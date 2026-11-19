#include "stdafx.h"
#include "Resource.h"
#include "ReadProgressDialog.h"
#include "Setting.h"
#include "MixedDisc.h"
#include <wincrypt.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <set>

namespace {
using Bytes = std::vector<unsigned char>;
constexpr int sectorBytes = 2352;
constexpr int offsetLimit = 12000 * 4;
constexpr int maxTrackBytes = 128 * 1024 * 1024;
constexpr int sqliteOk = 0;
constexpr int sqliteOpenReadonly = 1;
constexpr int sqliteRow = 100;

struct sqlite3;
struct sqlite3_stmt;

// The Windows 10 1607+ winsqlite3.dll exports use WINAPI on x86.
class ReadOnlyDb {
    HMODULE library = LoadLibraryExW(L"winsqlite3.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    sqlite3* db = nullptr;
    int (WINAPI* close)(sqlite3*) = nullptr;
public:
    int (WINAPI* prepare)(sqlite3*,const char*,int,sqlite3_stmt**,const char**) = nullptr;
    int (WINAPI* step)(sqlite3_stmt*) = nullptr;
    int (WINAPI* finalize)(sqlite3_stmt*) = nullptr;
    int (WINAPI* integer)(sqlite3_stmt*,int) = nullptr;
    const unsigned char* (WINAPI* text)(sqlite3_stmt*,int) = nullptr;

    bool open(const CString& path)
    {
        if (!library) return false;
        auto openReadOnly = reinterpret_cast<int (WINAPI*)(const char*,sqlite3**,int,const char*)>(
            GetProcAddress(library,"sqlite3_open_v2"));
        close = reinterpret_cast<decltype(close)>(GetProcAddress(library,"sqlite3_close"));
        prepare = reinterpret_cast<decltype(prepare)>(GetProcAddress(library,"sqlite3_prepare_v2"));
        step = reinterpret_cast<decltype(step)>(GetProcAddress(library,"sqlite3_step"));
        finalize = reinterpret_cast<decltype(finalize)>(GetProcAddress(library,"sqlite3_finalize"));
        integer = reinterpret_cast<decltype(integer)>(GetProcAddress(library,"sqlite3_column_int"));
        text = reinterpret_cast<decltype(text)>(GetProcAddress(library,"sqlite3_column_text"));
        if (!openReadOnly || !close || !prepare || !step || !finalize || !integer || !text) return false;
        CStringW wide(path);
        int length=WideCharToMultiByte(CP_UTF8,0,wide,-1,nullptr,0,nullptr,nullptr);
        if (!length) return false;
        std::vector<char> utf8(length);
        if (!WideCharToMultiByte(CP_UTF8,0,wide,-1,utf8.data(),length,nullptr,nullptr)) return false;
        return openReadOnly(utf8.data(),&db,sqliteOpenReadonly,nullptr)==sqliteOk;
    }

    bool query(const CString& sql,sqlite3_stmt*& statement) const
    {
        return prepare(db,sql,-1,&statement,nullptr)==sqliteOk;
    }

    ~ReadOnlyDb()
    {
        if (db) close(db);
        if (library) FreeLibrary(library);
    }
};

struct Statement {
    ReadOnlyDb& db;
    sqlite3_stmt* value = nullptr;
    ~Statement() { if (value) db.finalize(value); }
};

class Sha1 {
    HCRYPTPROV provider = 0;
public:
    Sha1() { CryptAcquireContext(&provider,nullptr,nullptr,PROV_RSA_FULL,CRYPT_VERIFYCONTEXT); }
    ~Sha1() { if (provider) CryptReleaseContext(provider,0); }
    bool matches(const unsigned char* bytes,size_t length,const std::string& expected) const
    {
        HCRYPTHASH hash=0;
        if (!provider || !CryptCreateHash(provider,CALG_SHA1,0,0,&hash)) return false;
        BYTE digest[20];
        DWORD size=sizeof(digest);
        bool ok=CryptHashData(hash,bytes,DWORD(length),0) && CryptGetHashParam(hash,HP_HASHVAL,digest,&size,0);
        CryptDestroyHash(hash);
        if (!ok || size!=sizeof(digest) || expected.size()!=2*size) return false;
        char hex[41];
        for (DWORD i=0;i<size;++i) std::snprintf(hex+2*i,3,"%02x",digest[i]);
        return _stricmp(hex,expected.c_str())==0;
    }
    bool matches(const Bytes& bytes,const std::string& expected) const
    {
        return matches(bytes.data(),bytes.size(),expected);
    }
};

struct DbTrack {
    int bytes;
    int start;
    std::string sha1;
    std::string crc;
};

bool readBytes(std::ifstream& file,int start,int length,Bytes& out)
{
    if (start<0 || length<=0 || length>maxTrackBytes+2*offsetLimit) return false;
    out.resize(length);
    file.clear();
    file.seekg(start);
    return bool(file.read(reinterpret_cast<char*>(out.data()),length));
}

std::vector<DbTrack> identify(ReadOnlyDb& db,std::ifstream& image,const Sha1& sha,int tracks,int bytes,int firstAudio)
{
    CString query;
    query.Format("SELECT nointro_game_id FROM nointro_rom WHERE lower(rom_name) LIKE '%%.bin' "
                 "GROUP BY nointro_game_id HAVING count(*)=%d AND sum(size)=%d",tracks,bytes);
    Statement ids{db};
    if (!db.query(query,ids.value)) return {};
    std::vector<DbTrack> match;
    Bytes data;
    while (db.step(ids.value)==sqliteRow) {
        query.Format("SELECT size,sha1,crc32 FROM nointro_rom WHERE nointro_game_id=%d "
                     "AND lower(rom_name) LIKE '%%.bin' ORDER BY id",db.integer(ids.value,0));
        Statement rows{db};
        if (!db.query(query,rows.value)) return {};
        std::vector<DbTrack> candidate;
        int start=0;
        while (db.step(rows.value)==sqliteRow) {
            int length=db.integer(rows.value,0);
            const auto* hash=db.text(rows.value,1);
            const auto* crc=db.text(rows.value,2);
            if (length<=0 || length%sectorBytes || length>maxTrackBytes || length>bytes-start || !hash) break;
            candidate.push_back({length,start,reinterpret_cast<const char*>(hash),
                                crc ? reinterpret_cast<const char*>(crc) : ""});
            start+=length;
        }
        if (start!=bytes || int(candidate.size())!=tracks || candidate[0].bytes>firstAudio*sectorBytes) continue;
        const auto& first=candidate.front();
        if (!readBytes(image,0,first.bytes,data) || !sha.matches(data,first.sha1)) continue;
        if (!match.empty()) return {};
        match=std::move(candidate);
    }
    return match;
}

// Rolling IEEE CRC-32 narrows offset candidates. SHA-1 decides a match.
using Crc = std::uint32_t;
using Matrix = std::array<Crc,32>;
const auto crcTable=[] {
    std::array<Crc,256> table{};
    for (Crc i=0;i<table.size();++i) {
        Crc r=i;
        for (int bit=0;bit<8;++bit) r=(r>>1)^((r&1)?0xedb88320u:0);
        table[i]=r;
    }
    return table;
}();

Crc crcByte(Crc value,unsigned char byte) { return (value>>8)^crcTable[(value^byte)&255]; }
Crc applyMatrix(const Matrix& m,Crc value)
{
    Crc result=0;
    for (int i=0;value;++i,value>>=1) if (value&1) result^=m[i];
    return result;
}

std::set<int> audioOffsets(std::ifstream& image,const Sha1& sha,const DbTrack& track)
{
    std::set<int> found;
    if (track.crc.size()!=8) return found;
    Bytes data;
    const int width=track.bytes;
    if (!readBytes(image,track.start-offsetLimit,width+2*offsetLimit,data)) return found;
    Matrix power{},shift{};
    for (int i=0;i<32;++i) {
        power[i]=crcByte(Crc(1)<<i,0);
        shift[i]=Crc(1)<<i;
    }
    for (int length=width;length;length>>=1) {
        if (length&1) for (auto& column:shift) column=applyMatrix(power,column);
        Matrix squared=power;
        for (auto& column:squared) column=applyMatrix(power,column);
        power=squared;
    }
    std::array<Crc,256> outgoing{};
    for (int i=0;i<256;++i) outgoing[i]=applyMatrix(shift,crcTable[i]);
    const Crc correction=~applyMatrix(shift,0xffffffffu);
    const Crc expected=std::strtoul(track.crc.c_str(),nullptr,16);
    Crc value=0;
    for (int i=0;i<width;++i) value=crcByte(value,data[i]);
    for (int i=0;i<=2*offsetLimit;++i) {
        if (i%4==0 && (value^correction)==expected && sha.matches(&data[i],width,track.sha1)) {
            found.insert(i-offsetLimit);
            if (found.size()>1) break;
        }
        if (i<2*offsetLimit) value=crcByte(value,data[i+width])^outgoing[data[i]];
    }
    return found;
}

bool confirmOffset(std::ifstream& image,const Sha1& sha,const std::vector<DbTrack>& tracks,
                   const bool& stopped,int& offset)
{
    std::vector<int> anchors;
    for (int i=2;i+1<int(tracks.size());++i) anchors.push_back(i);
    std::sort(anchors.begin(),anchors.end(),[&](int a,int b) { return tracks[a].bytes<tracks[b].bytes; });
    std::vector<int> votes(2*offsetLimit+1);
    int count=0;
    for (int i:anchors) {
        if (stopped || count++==8) break;
        auto found=audioOffsets(image,sha,tracks[i]);
        if (found.size()==1) ++votes[*found.begin()+offsetLimit];
    }
    auto best=std::max_element(votes.begin(),votes.end());
    if (*best<2 || std::count_if(votes.begin(),votes.end(),[](int n) { return n>=2; })!=1)
        return false;
    offset=int(best-votes.begin())-offsetLimit;
    return !stopped;
}

struct Repair { int start; Bytes bytes; };

CString saveVerifiedCopy(const CString& cue,const CString& image,const std::vector<Repair>& repairs)
{
    const CString target=cue.Left(cue.GetLength()-4)+"-verified";
    const CString temp=target+".part";
    if (GetFileAttributes(target)!=INVALID_FILE_ATTRIBUTES || !CreateDirectory(temp,nullptr)) return {};
    const CString cueName=cue.Mid(cue.ReverseFind('\\')+1);
    const CString newCue=temp+"\\"+cueName;
    const CString newImage=temp+"\\"+image.Mid(image.ReverseFind('\\')+1);
    bool ok=CopyFile(cue,newCue,TRUE) && CopyFile(image,newImage,TRUE);
    if (ok) {
        std::fstream file((LPCSTR)newImage,std::ios::binary|std::ios::in|std::ios::out);
        for (const auto& repair:repairs) {
            file.seekp(repair.start);
            file.write(reinterpret_cast<const char*>(repair.bytes.data()),repair.bytes.size());
        }
        file.close();
        ok=bool(file) && MoveFile(temp,target);
    }
    if (!ok) {
        DeleteFile(newCue);
        DeleteFile(newImage);
        RemoveDirectory(temp);
    }
    return ok ? target+"\\"+cueName : CString();
}
}

void CReadThread::VerifyRedump()
{
    const CString dbPath=theSetting.AvailableRedumpDb();
    if (!m_VerifyAfterRead || !theSetting.m_RedumpRetry || !theSetting.m_UseSPTI || dbPath.IsEmpty() ||
        theSetting.m_SwapChannel || theSetting.m_IgnoreError || theSetting.m_FastErrorSkip ||
        theSetting.m_TestReadMode || m_StopFlag) return;

    auto* toc=m_CD->GetTOC();
    if (toc->m_LastTrack<4 || toc->m_LastTrack>=99 || toc->m_Track[0].m_TrackType!=TRACKTYPE_DATA) return;
    std::vector<int> starts;
    for (int i=0;i<=toc->m_LastTrack;++i) {
        if (i<toc->m_LastTrack && (toc->m_Track[i].m_Session!=1 ||
            (i && toc->m_Track[i].m_TrackType!=TRACKTYPE_AUDIO))) return;
        starts.push_back(int(toc->m_Track[i].m_MSF.GetByLBA())-150);
        if (i && starts[i]<=starts[i-1]) return;
    }
    if (starts[0]!=0 || starts.back()>=450000) return;
    const int imageBytes=starts.back()*sectorBytes;
    std::ifstream image((LPCSTR)m_ImgPath,std::ios::binary|std::ios::ate);
    if (!image || image.tellg()!=imageBytes) return;
    auto* dlg=static_cast<CReadProgressDialog*>(m_ParentWnd);
    dlg->m_Message="Checking tracks against local Redump database...";
    dlg->PostMessage(WM_COMMAND,ID_UPDATE_DIALOG);
    m_LogWnd->AddMessage(LOG_INFO,dlg->m_Message);
    ReadOnlyDb db;
    if (!db.open(dbPath)) {
        m_LogWnd->AddMessage(LOG_WARNING,"Redump database could not be opened.");
        return;
    }
    Sha1 sha;
    auto tracks=identify(db,image,sha,toc->m_LastTrack,imageBytes,starts[1]);
    int offset=0;
    if (tracks.empty() || !confirmOffset(image,sha,tracks,m_StopFlag,offset)) {
        m_LogWnd->AddMessage(LOG_INFO,"Redump: release or audio offset could not be confirmed.");
        return;
    }
    CString line;
    line.Format("Redump: data SHA-1 and audio offset %+d samples confirmed.",offset/4);
    m_LogWnd->AddMessage(LOG_INFO,line);
    std::vector<Repair> repairs;
    std::vector<int> mismatches;
    Bytes original;
    for (int i=1;i<int(tracks.size()) && !m_StopFlag;++i) {
        const auto& track=tracks[i];
        const int at=track.start+offset;
        // The checker pads an audio window when it falls outside the image.
        original.assign(track.bytes,0);
        const int begin=(std::max)(0,at);
        const int end=(std::min)(imageBytes,at+track.bytes);
        Bytes actual;
        if (!readBytes(image,begin,end-begin,actual)) return;
        std::copy(actual.begin(),actual.end(),original.begin()+(begin-at));
        if (!sha.matches(original,track.sha1)) mismatches.push_back(i);
    }
    if (m_StopFlag) return;
    if (mismatches.empty()) {
        m_LogWnd->AddMessage(LOG_INFO,"Redump: all tracks match.");
        return;
    }

    if (!m_CD->ReadTOC() || toc->m_LastTrack+1!=starts.size()) return;
    for (int i=0;i<int(starts.size());++i)
        if (int(toc->m_Track[i].m_MSF.GetByLBA())-150!=starts[i] ||
            (i+1<int(starts.size()) && (toc->m_Track[i].m_Session!=1 ||
             (toc->m_Track[i].m_TrackType==TRACKTYPE_AUDIO)!=(i>0)))) return;

    for (int i:mismatches) {
        const auto& track=tracks[i];
        const int at=track.start+offset;
        const int first=at/sectorBytes, last=(at+track.bytes+sectorBytes-1)/sectorBytes;
        if (m_StopFlag || first<starts[1] || last>starts.back() ||
            !readBytes(image,at,track.bytes,original)) break;
        line.Format("Redump: rereading audio track %02d",i+1);
        m_LogWnd->AddMessage(LOG_INFO,line);
        dlg->m_Message=line;
        dlg->PostMessage(WM_COMMAND,ID_UPDATE_DIALOG);

        Bytes retry((last-first)*sectorBytes);
        bool recovered=false;
        int lead=at-first*sectorBytes;
        std::set<int> differences;
        constexpr size_t maxSuspectSectors=8;
        constexpr int maxSectorRereads=5;
        int sectorRereads=0;
        Bytes trial;
        auto trySector=[&](int lba,const unsigned char* sector) {
            const int begin=(std::max)(lba*sectorBytes,at);
            const int end=(std::min)((lba+1)*sectorBytes,at+track.bytes);
            if (trial.empty()) trial=original;
            const int sectorOffset=begin-lba*sectorBytes;
            const int trackOffset=begin-at;
            std::copy_n(sector+sectorOffset,end-begin,trial.begin()+trackOffset);
            if (sha.matches(trial,track.sha1)) {
                repairs.push_back({at,std::move(trial)});
                return true;
            }
            std::copy_n(original.begin()+trackOffset,end-begin,trial.begin()+trackOffset);
            return false;
        };
        for (int block:{24,17}) {
            int lba=first;
            for (;lba<last && !m_StopFlag;) {
                const int count=(std::min)(block,last-lba);
                if (!ReadVerificationAudio(lba,count,&retry[(lba-first)*sectorBytes],block==17)) break;
                lba+=count;
            }
            if (m_StopFlag) return;
            if (lba!=last) continue;
            if (sha.matches(&retry[lead],track.bytes,track.sha1)) {
                repairs.push_back({at,Bytes(retry.begin()+lead,retry.begin()+lead+track.bytes)});
                recovered=true;
                break;
            }
            std::set<int> passDifferences;
            for (int j=0;j<track.bytes;++j)
                if (original[j]!=retry[lead+j]) passDifferences.insert(first+(lead+j)/sectorBytes);
            differences.insert(passDifferences.begin(),passDifferences.end());
            // A fresh pass can fix the original sector and introduce a new error elsewhere.
            if (passDifferences.size()<=maxSuspectSectors) {
                for (int suspect:passDifferences) {
                    if (m_StopFlag) return;
                    if ((recovered=trySector(suspect,&retry[(suspect-first)*sectorBytes]))) break;
                }
                if (recovered) break;
            }
        }
        if (!recovered && !differences.empty() && differences.size()<=maxSuspectSectors) {
            Bytes sectors(24*sectorBytes);
            struct Attempt { int back, count; bool alternate; };
            const Attempt attempts[]={{0,1,false},{2,8,false},{8,24,false},
                                      {0,1,true},{2,8,true},{0,1,false},
                                      {8,24,true},{2,8,false}};
            const int seekCount=(std::min)(24,last-first);
            for (const auto& attempt:attempts) {
                for (int lba:differences) {
                    if (sectorRereads>=maxSectorRereads) break;
                    if (m_StopFlag) return;
                    ++sectorRereads;
                    // Displace the drive's read-ahead before trying this sector again.
                    const int seek=lba-first>last-lba ? first : last-seekCount;
                    ReadVerificationAudio(seek,seekCount,sectors.data());
                    const int from=(std::max)(starts[1],lba-attempt.back);
                    const int count=(std::min)(attempt.count,starts.back()-from);
                    if (!ReadVerificationAudio(from,count,sectors.data(),attempt.alternate)) continue;
                    if ((recovered=trySector(lba,&sectors[(lba-from)*sectorBytes]))) break;
                }
                if (recovered || sectorRereads>=maxSectorRereads) break;
            }
        }
        if (!recovered) {
            line.Format("Redump: track %02d could not be verified (%u differing sectors, %d sector rereads).",
                        i+1,static_cast<unsigned>(differences.size()),sectorRereads);
            m_LogWnd->AddMessage(LOG_WARNING,line);
            return;
        }
        line.Format("Redump: track %02d verified after rereading.",i+1);
        m_LogWnd->AddMessage(LOG_INFO,line);
    }
    image.close();
    if (m_StopFlag || repairs.size()!=mismatches.size()) return;
    const CString verifiedCue=saveVerifiedCopy(m_CuePath,m_ImgPath,repairs);
    if (!verifiedCue.IsEmpty()) {
        m_ChdCue=verifiedCue;
        m_LogWnd->AddMessage(LOG_NORMAL,"Redump: all tracks match; saved the -verified copy.");
    } else
        m_LogWnd->AddMessage(LOG_WARNING,"Redump: could not save a new -verified copy.");
}
