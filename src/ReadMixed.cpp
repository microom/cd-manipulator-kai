#include "stdafx.h"
#include "Resource.h"
#include "ReadProgressDialog.h"
#include "SptiDriver.h"
#include "Setting.h"
#include "MixedDisc.h"
#include <fstream>
#include <algorithm>

namespace {
const char* GetMixedLayout(TableOfContents& toc,std::vector<mixed::Track>& tracks,int& end)
{
    if (!toc.m_LastTrack || toc.m_LastTrack >= 99) return "Unsupported track count.";
    for (int i=0;i<toc.m_LastTrack;++i) {
        auto& track=toc.m_Track[i];
        const int start=int(track.m_MSF.GetByLBA())-150;
        if (track.m_Session!=1 || track.m_TrackNo!=i+1 ||
            (i==0 ? start!=0 : start<=tracks.back().start))
            return "Expected a single session with consecutive tracks starting at LBA 0.";
        tracks.push_back({start,track.m_TrackType==TRACKTYPE_AUDIO ? 0 : -1});
    }
    end=int(toc.m_Track[toc.m_LastTrack].m_MSF.GetByLBA())-150;
    return end<=tracks.back().start || end>=450000 ? "Invalid lead-out." : nullptr;
}

class NewFile {
public:
    HANDLE h = INVALID_HANDLE_VALUE;
    bool open(const CString& name) {
        h = CreateFile(name,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
        return h != INVALID_HANDLE_VALUE;
    }
    bool write(const void* bytes,DWORD length) {
        DWORD done = 0;
        return WriteFile(h,bytes,length,&done,NULL) && done == length;
    }
    bool finish() {
        BOOL flushed = FlushFileBuffers(h);
        BOOL closed = CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
        return flushed && closed;
    }
    ~NewFile() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};

class MixedIO {
    static constexpr int batchSectors=24; // 56,448 bytes, below the 64 KiB USB transfer limit.
    static constexpr int tailSectors=6;
    static constexpr int tailOverlap=2;
    CSptiDriver& drive;
    CReadThread& worker;
    CReadProgressDialog& dlg;
    NewFile& image;
    const int trackCount;
    const int leadout;
    int percent = -1;
    int preferred = 0;
    int preferredQ = 0;
    int qOffset[4] = {};
    CString qDetail;
    bool mediumChanged = false;
    bool mode1ReadError = false;
    bool audioBoundaryReadError = false;
    bool audioBoundaryQError = false;
    enum { Audio, Raw, Mode1, Mode2 };
    static constexpr int types[] = {1,0,2,3};
    static constexpr int fields[] = {0x10,0xf8,0xb8,0xf8};
    alignas(16) std::array<mixed::Sector,batchSectors> batch;
    int firstBuffered=0;
    int buffered=0;
    int batchCommands=0;
    int batchedFrames=0;
    bool batchFailed=false;
    int repositionAt=-1;
    int repositionInside=0;
    int repositionMethod=-1;

    bool issue(SRB_ExecSCSICmd& cmd,DWORD timeout) {
        if (drive.ExactRead(cmd,timeout)) return true;
        if ((cmd.SenseArea[2]&15) == 6 && cmd.SenseArea[12] == 0x28)
            mediumChanged = true;
        return false;
    }

    bool command(int lba,int type,int fields,int sub,BYTE* dst,DWORD size,SRB_ExecSCSICmd& cmd,int frames=1) {
        memset(&cmd,0,sizeof(cmd));
        cmd.SRB_BufLen = size;
        cmd.SRB_BufPointer = dst;
        cmd.SRB_CDBLen = 12;
        cmd.SRB_Flags = SRB_DIR_IN;
        cmd.CDBByte[0] = 0xbe;
        cmd.CDBByte[1] = BYTE(type<<2);
        for (int i=0;i<4;++i) cmd.CDBByte[5-i] = BYTE(unsigned(lba)>>(8*i));
        cmd.CDBByte[7] = BYTE(frames>>8);
        cmd.CDBByte[8] = BYTE(frames);
        cmd.CDBByte[9] = BYTE(fields);
        cmd.CDBByte[10] = BYTE(sub);
        const DWORD timeout=frames>1 && !sub ? 15 : 0xffff;
        return issue(cmd,timeout);
    }
    void seekData(int lba) {
        if (stopped()) return;
        std::array<BYTE,2048> payload;
        SRB_ExecSCSICmd cmd{};
        cmd.SRB_BufLen=DWORD(payload.size());
        cmd.SRB_BufPointer=payload.data();
        cmd.SRB_CDBLen=10;
        cmd.SRB_Flags=SRB_DIR_IN;
        cmd.CDBByte[0]=0x28; // READ (10), inside the data track.
        for (int i=0;i<4;++i) cmd.CDBByte[5-i]=BYTE(unsigned(lba)>>(8*i));
        cmd.CDBByte[8]=1;
        issue(cmd,15);
    }
    bool readDataPrefix(int lba,int inside,mixed::Sector& sector) {
        SRB_ExecSCSICmd cmd;
        auto read = [&](int method) {
            return !stopped() && command(lba,types[method],fields[method],0,sector.data(),2352,cmd) &&
                   mixed::dataHeader(sector,lba);
        };
        if (read(Mode1)) return true;
        seekData(inside);
        if (!stopped()) {
            mixed::Sector warmup;
            command(inside,types[Mode1],fields[Mode1],0,warmup.data(),2352,cmd);
        }
        return read(Mode1) || read(Raw) || read(Mode2);
    }
    bool readBatch(int lba,int count,int method) {
        SRB_ExecSCSICmd cmd;
        return command(lba,types[method],fields[method],0,batch.front().data(),
                       DWORD(count*sizeof(mixed::Sector)),cmd,count);
    }
    bool fromBatch(int lba,mixed::Sector& out) const {
        if (lba<firstBuffered || lba-firstBuffered>=buffered) return false;
        out=batch[lba-firstBuffered];
        return true;
    }
public:
    MixedIO(CSptiDriver& d,CReadThread& w,CReadProgressDialog& p,NewFile& file,int tracks,int discEnd)
        : drive(d),worker(w),dlg(p),image(file),trackCount(tracks),leadout(discEnd) {}
    bool stopped() { return worker.m_StopFlag || mediumChanged; }
    bool readAudioTail(int lba,int trackStart,int end,mixed::Sector& out) {
        if (fromBatch(lba,out)) return true;
        buffered=0;
        const int overlap=(std::min)(lba-trackStart,tailOverlap);
        const int count=(std::min)(end-lba,tailSectors);
        if (overlap>0 && readBatch(lba-overlap,overlap+count,Audio)) {
            firstBuffered=lba-overlap;
            buffered=overlap+count;
            return fromBatch(lba,out);
        }
        if (stopped()) return false;
        SRB_ExecSCSICmd cmd;
        if (command(lba,1,0x10,0,out.data(),2352,cmd)) return true;
        CString msg;
        msg.Format("Audio tail stopped at LBA %d (failed or short; sense %02X/%02X/%02X)",
                   lba,cmd.SenseArea[2]&15,cmd.SenseArea[12],cmd.SenseArea[13]);
        worker.m_LogWnd->AddMessage(LOG_INFO,msg);
        return false;
    }
    void tail(int tr,int first,int frames) {
        CString msg;
        msg.Format("Track %02d audio tail: %d sector(s), LBA [%d,%d)",tr,frames,first,first+frames);
        worker.m_LogWnd->AddMessage(LOG_INFO,msg);
    }
    bool changed() const { return mediumChanged; }
    mixed::AudioGap probeEarlyDataPrefix(int first,int start,int at,int end,int nextEnd,int track,
                                        const std::vector<BYTE>& decoded) {
        const auto frameAt=[&](int lba) {
            mixed::QFrame frame;
            memcpy(frame.data(),&decoded[(lba-start)*16],frame.size());
            return frame;
        };
        const int prefixSize=mixed::earlyDataFrames(frameAt,at,end,track+1);
        if (!prefixSize) return {};
        const int prefixStart=end-prefixSize;
        const int evidenceStart=at-1;
        const int count=prefixStart-evidenceStart+1; // Include the first verified INDEX 01.
        std::vector<mixed::QFrame> evidence(count), frames(count), previous;
        for (int i=0;i<count;++i)
            memcpy(frames[i].data(),&decoded[(evidenceStart+i-start)*16],frames[i].size());
        int repairs=0;
        if (!mixed::mergeGapFrames(evidence,frames,previous,evidenceStart,track+1,repairs)) return {};

        for (int probe : {start,(std::min)(start+7,evidenceStart),(std::max)(first,start-9)}) {
            if (mixed::streamConfirmsGap(evidence,at,track)) break;
            const int framesToRead=end-probe+1;
            if (stopped() || framesToRead*96>65536) continue;
            std::vector<BYTE> raw(framesToRead*96);
            SRB_ExecSCSICmd cmd;
            if (!command(probe,0,0,1,raw.data(),DWORD(raw.size()),cmd,framesToRead)) continue;
            for (int i=0;i<count;++i)
                frames[i]=mixed::unpackQ(&raw[(evidenceStart+i-probe)*96]);
            if (!mixed::mergeGapFrames(evidence,frames,previous,evidenceStart,track+1,repairs)) return {};
            previous=frames;
        }
        mixed::AudioGap gap;
        if (stopped() || !mixed::streamConfirmsGap(evidence,at,track,&gap.firstData)) return {};
        const int inside=(std::min)(end+75,nextEnd-1);
        gap.dataPrefix.resize(prefixSize);
        for (int i=0;i<prefixSize;++i) {
            auto& sector=gap.dataPrefix[i];
            if (!readDataPrefix(prefixStart+i,inside,sector) || sector[15]!=gap.dataPrefix.front()[15])
                return {};
        }
        gap.frames=end-at;
        repositionAt=end;
        repositionInside=inside;
        repositionMethod=gap.dataPrefix.front()[15]==1 ? Mode1 : Mode2;
        CString line;
        line.Format("Track %02d: SubQ verifies INDEX 00 LBA [%d,%d), preserving data LBA [%d,%d) before TOC (%d Q repairs)",
                    track+1,at,prefixStart,prefixStart,end,repairs);
        worker.m_LogWnd->AddMessage(LOG_INFO,line);
        return gap;
    }
    mixed::AudioGap findAudioGap(int first,int end,int nextEnd,int track) {
        // A different starting LBA can avoid a dropped position report.
        // Every proposed start still has to pass the complete gap check.
        for (int window : {300,450,750,mixed::maxVerifiedGap}) {
            if (stopped()) break;
            const int start=(std::max)(first,end-window-1);
            const int count=end-start+1;
            std::vector<BYTE> raw(count*16);
            SRB_ExecSCSICmd cmd;
            if (command(start,0,0,2,raw.data(),DWORD(raw.size()),cmd,count)) {
                for (int at=start+1;at<end;++at) {
                    const BYTE* p=&raw[(at-start)*16];
                    if (mixed::position(p-16,at-1,track,1,false) &&
                        mixed::position(p,at,track+1,0,(p[0]&0x40)!=0)) {
                        auto gap=probeEarlyDataPrefix(first,start,at,end,nextEnd,track,raw);
                        if (gap.frames) return gap;
                        if (streamGap(first,at,end,track,&gap.firstData)) {
                            CString msg;
                            msg.Format("Track %02d INDEX 00 switches to data at LBA %d",track+1,gap.firstData);
                            worker.m_LogWnd->AddMessage(LOG_INFO,msg);
                            gap.frames=end-at;
                            return gap;
                        }
                    }
                }
                CString msg;
                msg.Format("Track %02d: pregap not confirmed in SubQ window from LBA %d",track+1,start);
                worker.m_LogWnd->AddMessage(LOG_INFO,msg);
            } else {
                CString msg;
                msg.Format("Track %02d: SubQ window from LBA %d failed/short, sense %02X/%02X/%02X",
                           track+1,start,cmd.SenseArea[2]&15,cmd.SenseArea[12],cmd.SenseArea[13]);
                worker.m_LogWnd->AddMessage(LOG_INFO,msg);
            }
            if (start==first) break;
        }
        return {};
    }
    bool streamGap(int trackStart,int failed,int end,int track,int* firstData=nullptr) {
        std::vector<mixed::QFrame> evidence(end-failed+2);
        std::vector<mixed::QFrame> rawSeen(evidence.size());
        int repaired=0;
        for (int back : {1,3,8,16,32,64,128}) {
            int start=failed-back;
            if (stopped() || start<trackStart) break;
            int count=end-start+1;
            std::vector<BYTE> raw(count*16);
            SRB_ExecSCSICmd cmd;
            if (!command(start,0,0,2,raw.data(),DWORD(raw.size()),cmd,count)) {
                CString line;
                line.Format("SubQ stream from %d (%d frames): failed/short sense %02X/%02X/%02X",
                            start,count,cmd.SenseArea[2]&15,cmd.SenseArea[12],cmd.SenseArea[13]);
                worker.m_LogWnd->AddMessage(LOG_INFO,line);
                continue;
            }
            for (int at=failed-1;at<=end;++at) {
                mixed::QFrame q;
                memcpy(q.data(),&raw[(at-start)*16],q.size());
                if (!mixed::addressedStreamQ(q,at)) continue;
                auto& slot=evidence[at-failed+1];
                if (slot[0] && slot!=q) return false;
                slot=q;
            }
            // Use raw P-W only where the drive's decoded Q was missing or
            // repeated a different address. Keep SPTI transfers below 64 KiB.
            if (!mixed::streamConfirmsGap(evidence,failed,track) && count*96<=65536 && !stopped()) {
                raw.resize(count*96);
                if (command(start,0,0,1,raw.data(),DWORD(raw.size()),cmd,count)) {
                    for (int at=failed;at<end;++at) {
                        int i=at-failed+1;
                        if (evidence[i][0]) continue;
                        auto q=mixed::unpackQ(&raw[(at-start)*96]);
                        if (mixed::addressedStreamQ(q,at)) evidence[i]=q;
                        else if (rawSeen[i]==q && mixed::gapQFromNeighbours(q,evidence[i-1],evidence[i+1],
                                 at,track+1,evidence[i])) ++repaired;
                        else rawSeen[i]=q;
                    }
                }
            }
            if (mixed::streamConfirmsGap(evidence,failed,track,firstData)) {
                CString msg;
                msg.Format("Track %02d INDEX 00 verified by continuous SubQ: LBA [%d,%d), %d single-bit Q repairs",track+1,failed,end,repaired);
                worker.m_LogWnd->AddMessage(LOG_INFO,msg);
                return true;
            }
        }
        return false;
    }
    void begin(int track,int first,int end) {
        preferred = -1;
        buffered=0;
        batchFailed=false;
        qDetail.Empty();
        dlg.m_Message.Format("Track %02d  LBA %d .. %d",track,first,end-1);
        worker.m_LogWnd->AddMessage(LOG_INFO,dlg.m_Message);
        dlg.SetTrackProgress(track,trackCount);
        dlg.PostMessage(WM_COMMAND,ID_UPDATE_DIALOG);
        if (first==repositionAt) {
            seekData(repositionInside);
            preferred=repositionMethod;
            repositionAt=-1;
        }
    }
    bool read(int lba,int mode,int end,mixed::Sector& out) {
        mode1ReadError=false;
        audioBoundaryReadError=false;
        if (fromBatch(lba,out)) return true;
        buffered=0;
        // Read the optional final MODE1 sector separately so its failure
        // cannot discard the preceding batch.
        const bool isolateLast=mode==1 && end==leadout && theSetting.m_ZeroFillFinalSector;
        const int batchEnd=isolateLast ? end-1 : end;
        const bool readingLast=isolateLast && lba==batchEnd;
        const int count=(std::min)(batchEnd-lba,batchSectors);
        const bool canBatch=mode==0 ? preferred==Audio : mode>0 && preferred>=Raw;
        if (!batchFailed && canBatch && count>1) {
            ++batchCommands;
            if (readBatch(lba,count,preferred)) {
                firstBuffered=lba;
                // A data-to-audio pregap may start before the next TOC entry.
                // Return only the confirmed data prefix and let single reads
                // handle the rest, including a change of sector type.
                for (;buffered<count;++buffered) {
                    const auto& sector=batch[buffered];
                    if (mode!=0 && (!mixed::dataHeader(sector,lba+buffered) || sector[15]!=mode)) break;
                }
                batchedFrames+=buffered;
            }
            batchFailed=buffered!=count;
            if (fromBatch(lba,out)) return true;
        }
        if (readingLast) {
            CString msg;
            msg.Format("Reading final MODE1 sector at LBA %d separately.",lba);
            worker.m_LogWnd->AddMessage(LOG_INFO,msg);
        }
        const int first=readingLast && preferred!=Mode1 ? Raw : preferred;
        const int choices[] = {first,mode==0 ? Audio : Raw,Raw,Mode1,Mode2,Audio};
        constexpr unsigned mode1Methods=(1u<<Raw)|(1u<<Mode1);
        unsigned uncorrectable=0;
        unsigned tried=0;
        for (int choice : choices) {
            if (stopped()) return false;
            if (choice<0 || (tried & (1u<<choice))) continue;
            tried |= 1u<<choice;
            SRB_ExecSCSICmd cmd;
            if (command(lba,types[choice],fields[choice],0,out.data(),2352,cmd)) {
                preferred = choice;
                return true;
            }
            const unsigned method=1u<<choice;
            if (mode==1 && (cmd.SenseArea[2]&15)==3 && cmd.SenseArea[12]==0x11) {
                if (method & mode1Methods) mode1ReadError=true;
                if (cmd.SenseArea[13]==0x05) uncorrectable|=method;
            }
            CString msg;
            msg.Format("READ CD LBA %d type %d fields %02X: failed or short; sense %02X/%02X/%02X",
                lba,types[choice],fields[choice],cmd.SenseArea[2]&15,cmd.SenseArea[12],cmd.SenseArea[13]);
            worker.m_LogWnd->AddMessage(LOG_WARNING,msg);
            if (readingLast && (uncorrectable & mode1Methods)==mode1Methods) break;
        }
        const unsigned boundaryMethods=mode1Methods|(1u<<Audio);
        audioBoundaryReadError=(uncorrectable & boundaryMethods)==boundaryMethods;
        return false;
    }
    bool audioPregapReadError() const { return audioBoundaryReadError; }
    bool audioPregapQError() const { return audioBoundaryQError; }
    void filledAudioPregap(int track,int lba) {
        if (!qDetail.IsEmpty()) worker.m_LogWnd->AddMessage(LOG_WARNING,qDetail);
        CString line;
        line.Format("Track %02d INDEX 00 LBA %d: zero-filled 1 sector; preserved the remaining %d audio sectors.",
                    track,lba,mixed::audioPregapFrames-1);
        worker.m_LogWnd->AddMessage(LOG_WARNING,line);
        const bool japanese=theSetting.m_LangFile.Right(12).CompareNoCase("Japanese.cml")==0;
        dlg.m_CompletionDetail=CString(japanese
            ? L"\u97f3\u58f0\u30d7\u30ea\u30ae\u30e3\u30c3\u30d7\u306e\u5148\u982d1\u30bb\u30af\u30bf\u30920\u3067\u88dc\u5b8c\u3057\u307e\u3057\u305f\u3002"
            : L"The first audio pregap sector was zero-filled.");
    }
    bool zeroFillFinalSector(int track,int lba,mixed::Sector& out) {
        if (stopped() || !theSetting.m_ZeroFillFinalSector || !mode1ReadError) return false;
        out.fill(0);
        CString line;
        line.Format("Track %02d LBA %d: unreadable final MODE1 sector replaced with 2352 zero bytes.",track,lba);
        worker.m_LogWnd->AddMessage(LOG_WARNING,line);
        const bool japanese=theSetting.m_LangFile.Right(12).CompareNoCase("Japanese.cml")==0;
        if (!dlg.m_CompletionDetail.IsEmpty()) dlg.m_CompletionDetail+=L"\r\n";
        dlg.m_CompletionDetail+=CString(japanese
            ? L"\u8aad\u3081\u306a\u3044\u6700\u7d42\u30bb\u30af\u30bf\u30920\u3067\u88dc\u5b8c\u3057\u307e\u3057\u305f\u3002"
            : L"The unreadable final sector was zero-filled.");
        return true;
    }
    void reportBatches() {
        CString msg;
        msg.Format("Program reads: %d sectors from %d batch requests (up to %d sectors/request)",
                   batchedFrames,batchCommands,batchSectors);
        worker.m_LogWnd->AddMessage(LOG_INFO,msg);
    }
    bool q(int lba,int track,int index,bool data) {
        struct Format { int fields,sub; const char* name; };
        static const Format formats[] = {{0,2,"Q16"},{0,1,"PW96"},{0xf8,1,"raw+PW96"},{0xf8,2,"raw+Q16"}};
        qDetail.Format("SubQ LBA %d expected T%02d INDEX %02d (%s):",lba,track,index,data ? "data" : "audio");
        audioBoundaryQError=false;
        bool received=false;
        BYTE reply[2352+96];
        for (int attempt=0; attempt<4; ++attempt) {
            if (stopped()) return false;
            const int method = (preferredQ+attempt)%4;
            const Format& fmt = formats[method];
            const int mainBytes = fmt.fields ? 2352 : 0;
            auto read = [&](int at,mixed::QFrame& frame) {
                if (stopped()) return false;
                SRB_ExecSCSICmd cmd;
                CString detail;
                if (!command(at,0,fmt.fields,fmt.sub,reply,mainBytes+(fmt.sub==1 ? 96 : 16),cmd)) {
                    if (at==lba && (cmd.SenseArea[2]&15)==3 && cmd.SenseArea[12]==0x11 && cmd.SenseArea[13]==0x05)
                        audioBoundaryQError=true;
                    detail.Format(" %s@%d failed/short sense %02X/%02X/%02X",fmt.name,at,
                        cmd.SenseArea[2]&15,cmd.SenseArea[12],cmd.SenseArea[13]);
                    qDetail += detail;
                    return false;
                }
                received=true;
                if (fmt.sub==1) frame = mixed::unpackQ(reply+mainBytes);
                else memcpy(frame.data(),reply+mainBytes,frame.size());
                detail.Format(" %s@%d [%02X %02X %02X abs %02X:%02X:%02X crc %02X%02X]",
                    fmt.name,at,frame[0],frame[1],frame[2],frame[7],frame[8],frame[9],frame[10],frame[11]);
                qDetail += detail;
                return true;
            };
            int previousOffset = qOffset[method];
            auto match = mixed::matchQ(lba,track,index,data,qOffset[method],read);
            if (match!=mixed::QMatch::Unknown) {
                audioBoundaryQError=false;
                if (previousOffset != qOffset[method]) {
                    CString msg;
                    msg.Format("SubQ %s request offset %d verified at LBA %d",fmt.name,qOffset[method],lba);
                    worker.m_LogWnd->AddMessage(LOG_INFO,msg);
                }
                preferredQ = method;
                if (match==mixed::QMatch::Catalogue) {
                    CString msg;
                    msg.Format("SubQ catalogue at LBA %d: T%02d INDEX 00 confirmed on both sides",lba,track);
                    worker.m_LogWnd->AddMessage(LOG_INFO,msg);
                }
                return match!=mixed::QMatch::Different;
            }
        }
        audioBoundaryQError=audioBoundaryQError && !received;
        return false;
    }
    bool write(const mixed::Sector& s) { return image.write(s.data(),2352); }
    bool trim(int frames) {
        LARGE_INTEGER pos;
        pos.QuadPart = LONGLONG(frames)*2352;
        return SetFilePointerEx(image.h,pos,NULL,FILE_BEGIN) && SetEndOfFile(image.h);
    }
    void gap(int tr,int start,int end,bool reference) {
        CString msg;
        msg.Format("Track %02d PREGAP: omitted LBA [%d,%d), %d frames (%s)",tr,start,end,
                   end-start,reference ? "reference CUE + live TOC" : "confirmed SubQ INDEX 00");
        worker.m_LogWnd->AddMessage(LOG_WARNING,msg);
    }
    bool fail(int tr,int lba,LPCSTR why) {
        if (!qDetail.IsEmpty() && strcmp(why,"sector read failed")==0)
            worker.m_LogWnd->AddMessage(LOG_WARNING,qDetail);
        dlg.m_Message.Format("Track %02d LBA %d: %s",tr,lba,why);
        worker.m_LogWnd->AddMessage(LOG_ERROR,dlg.m_Message);
        return false;
    }
    void progress(int lba,int leadout) {
        int p = 100*lba/leadout;
        if (p == percent) return;
        percent = p;
        dlg.m_Percent.Format("%d%%",p);
        dlg.m_Progress.SetPos(p);
        dlg.PostMessage(WM_COMMAND,ID_UPDATE_DIALOG);
    }
};
}

bool CReadThread::CanAutoReadMixed(TableOfContents& toc)
{
    if (!theSetting.m_UseSPTI || theSetting.m_ReadEngine!=0 ||
        theSetting.m_TestReadMode || theSetting.m_AnalyzeSubQ ||
        theSetting.m_IgnoreError || theSetting.m_FastErrorSkip || theSetting.m_BurstErrorScan ||
        theSetting.m_SwapChannel || !theSetting.m_AutoDetectMethod ||
        theSetting.m_Speed_Audio!=0xff || theSetting.m_Speed_Data!=0xff) return false;
    std::vector<mixed::Track> tracks;
    int end;
    if (GetMixedLayout(toc,tracks,end)) return false;
    return std::adjacent_find(tracks.begin(),tracks.end(),[](const mixed::Track& current,const mixed::Track& next) {
        return current.mode==0 && next.mode!=0;
    })!=tracks.end();
}

DWORD CReadThread::ReadMixedDisc() {
    CReadProgressDialog& dlg = *static_cast<CReadProgressDialog*>(m_ParentWnd);
    auto fail = [&](LPCSTR why) {
        dlg.m_Message = why;
        m_LogWnd->AddMessage(LOG_ERROR,why);
        return DWORD(0);
    };
    if (!theSetting.m_UseSPTI || !m_CD->ReadTOC()) return fail("Cannot read current disc TOC via SPTI.");
    std::vector<mixed::Track> tracks;
    int end;
    if (const char* error=GetMixedLayout(*m_CD->GetTOC(),tracks,end)) return fail(error);
    bool hasReference = !m_LayoutFile.IsEmpty();
    if (hasReference) {
        std::ifstream ref((LPCSTR)m_LayoutFile);
        std::string name;
        if (!ref || !mixed::reference(ref,tracks,name)) return fail("Reference CUE unsupported or track layout differs from disc.");
        CString dir = m_LayoutFile.Left(m_LayoutFile.ReverseFind('\\')+1);
        WIN32_FILE_ATTRIBUTE_DATA info;
        if (!GetFileAttributesEx(dir+name.c_str(),GetFileExInfoStandard,&info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || info.nFileSizeHigh ||
            info.nFileSizeLow != DWORD(end-mixed::omittedCount(tracks))*2352)
            return fail("Reference BIN missing or size differs from disc lead-out.");
    }
    CString cue = m_FileName;
    if (cue.Right(4).CompareNoCase(".cue") != 0) return fail("Choose a new .cue file.");
    CString img = cue.Left(cue.GetLength()-4)+".img";
    CString part = img+".part", cuePart = cue+".part";
    if (GetFileAttributes(img)!=INVALID_FILE_ATTRIBUTES || GetFileAttributes(cue)!=INVALID_FILE_ATTRIBUTES ||
        GetFileAttributes(cuePart)!=INVALID_FILE_ATTRIBUTES) return fail("Output exists. Choose a new name.");
    NewFile image;
    if (!image.open(part)) return fail("Cannot create new .img.part. Choose a new name.");
    MixedIO io(*static_cast<CSptiDriver*>(m_CD->GetAspiCtrl()),*this,dlg,image,int(tracks.size()),end);
    dlg.m_Progress.SetRange(0,100);
    m_LogWnd->AddMessage(LOG_INFO,hasReference ? "PCE-CD: reference layout" : "PCE-CD: disc layout");
#ifdef CDM_SKIP_AUDIO_TAIL
    constexpr bool recoverAudioTail=false;
    m_LogWnd->AddMessage(LOG_INFO,"Additional CD-DA reads in confirmed pregaps disabled.");
#else
    constexpr bool recoverAudioTail=true;
#endif
    if (!mixed::acquire(tracks,end,hasReference,io,recoverAudioTail)) {
        if (io.changed()) return fail("Disc changed during read. Discard the partial image.");
        return 0;
    }
    io.reportBatches();
    LARGE_INTEGER size;
    if (!GetFileSizeEx(image.h,&size) || size.QuadPart != LONGLONG(end-mixed::omittedCount(tracks))*2352 ||
        !image.finish()) return fail("Incomplete image on disk.");
    const std::string imageName(img.Mid(img.ReverseFind('\\')+1).GetString());
    const std::string contents=mixed::cue(imageName,tracks);
    NewFile out;
    if (m_StopFlag) return 0;
    if (!out.open(cuePart) || !out.write(contents.data(),DWORD(contents.size())) || !out.finish())
        return fail("Cannot save complete CUE.");
    if (!MoveFile(part,img) || !MoveFile(cuePart,cue)) return fail("Cannot publish IMG/CUE. Check output directory.");
    m_CuePath=cue;
    m_ChdCue=cue;
    if (m_CreateChdAfterRead && std::any_of(tracks.begin(),tracks.end(),[](const mixed::Track& track) {
        return track.omitted>0 && track.storedGap>0;
    })) {
        std::ifstream saved((LPCSTR)img,std::ios::binary);
        if (saved && mixed::isPceCd(saved,tracks,end))
            m_ChdCueText=mixed::cue(imageName,tracks,true);
    }
    return 1;
}
