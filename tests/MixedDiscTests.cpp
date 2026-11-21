#include "MixedDisc.h"
#include <algorithm>
#include <direct.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)

struct FakeDrive {
    int preconfirmed=0, prefixAt=-1, prefixCount=1, firstData=11, silentUntil=-1;
    int unreadable = 9, badQ = -1, writeFail = -1;
    int audioEnd = 9, headerAt = -1, last = -1, cancelAt = -1;
    std::vector<int> kept, probes;
    bool trimOK = true, cancel = false;
    bool allowFinalFill = false, dataReadError = true;
    int filledLba = -1;
    bool zeroFillFinalSector(int,int lba,mixed::Sector& s) {
        if (!allowFinalFill || !dataReadError || stopped()) return false;
        filledLba=lba;
        s.fill(0);
        return true;
    }
    bool stopped() { return cancel; }
    bool streamGap(int,int,int,int) { return false; }
    mixed::AudioGap findAudioGap(int,int,int,int) {
        mixed::AudioGap gap{preconfirmed,firstData};
        if (prefixAt>=0) {
            gap.dataPrefix.resize(prefixCount);
            for (int i=0;i<prefixCount;++i)
                gap.dataPrefix[i].back()=static_cast<unsigned char>(prefixAt+i);
        }
        return gap;
    }
    void begin(int,int,int) {}
    void progress(int,int) {}
    void gap(int,int,int,bool) {}
    void tail(int,int,int) {}
    bool readAudioTail(int lba,int,int,mixed::Sector& s) {
        probes.push_back(lba);
        if (lba==cancelAt) cancel=true;
        if (cancel || lba>=audioEnd || !read(lba,0,0,s)) return false;
        if (lba<silentUntil) s.fill(0);
        return true;
    }
    bool fail(int,int,const char*) { return false; }
    bool trim(int n) { if (!trimOK) return false; kept.resize(n); return true; }
    bool q(int lba,int tr,int idx,bool data) {
        if (lba == badQ) return false;
        return lba < 8 ? tr==1 && idx==1 && !data : tr==2 && idx==(lba>=12) && data;
    }
    bool write(const mixed::Sector& s) {
        if (s.back()==writeFail) return false;
        kept.push_back(s.back());
        return true;
    }
    bool read(int lba,int,int,mixed::Sector& s) {
        last=lba;
        if (lba==unreadable) return false;
        s.fill(0);
        if (lba>=12 || lba==headerAt) {
            std::fill(s.begin()+1,s.begin()+11,255);
            s[12]=0; s[13]=mixed::bcd((lba+150)/75); s[14]=mixed::bcd((lba+150)%75); s[15]=1;
            if (lba==headerAt) s[14]=0; // reject a data header even at an unexpected address
        }
        s.back()=(unsigned char)lba;
        return true;
    }
};
void catalogGapTests() {
    using mixed::QFrame;
    using mixed::QMatch;
    // The middle and following packets reproduce the fields and CRCs in the
    // r6 log. The preceding packet is a consistent INDEX 00 test witness.
    const QFrame before{{0x41,2,0,0,1,1,0,0,0x48,0x63,0x13,0x96}};
    const QFrame catalog{{0x42,0,0,0,0,0,0,0,0,0x64,0x7f,0x4d}};
    const QFrame after{{0x41,2,0,0,0,0x74,0,0,0x48,0x65,0x08,0x21}};
    QFrame left=before, middle=catalog, right=after;
    int shift=1;
    auto probe = [&](int at,QFrame& out) {
        switch(at+shift) {
            case 3513: out=left; return true;
            case 3514: out=middle; return true;
            case 3515: out=right; return true;
            default: return false;
        }
    };
    int offset=-1;
    auto match=[&](){return mixed::matchQ(3514,2,0,true,offset,probe);};
    CHECK(match()==QMatch::Catalogue);
    CHECK(mixed::matchQ(3514,2,1,true,offset,probe)==QMatch::Unknown);
    CHECK(mixed::matchQ(3514,1,0,false,offset,probe)==QMatch::Unknown);
    shift=0; offset=0; CHECK(match()==QMatch::Catalogue);
    auto seal=[](QFrame& q) {
        unsigned crc=0;
        for(int i=0;i<10;++i) for(int bit=7;bit>=0;--bit) {
            bool xorBit=((crc>>15)^(q[i]>>bit))&1;
            crc=(crc<<1)^(xorBit?0x1021:0);
        }
        q[10]=(unsigned char)(~crc>>8); q[11]=(unsigned char)~crc;
    };
    middle[10]^=1; CHECK(match()==QMatch::Unknown);
    middle=catalog; middle[9]=0x65; seal(middle); CHECK(match()==QMatch::Unknown);
    middle=catalog; middle[0]=0x40; seal(middle); CHECK(match()==QMatch::Unknown);
    middle=catalog; right[2]=1; seal(right); CHECK(match()==QMatch::Unknown);
    right=after; right[5]=0x73; seal(right); CHECK(match()==QMatch::Unknown);
    right=after; left[11]^=1; CHECK(match()==QMatch::Unknown);
    left=before; right=catalog; CHECK(match()==QMatch::Unknown);

    // No reference CUE. Simulate a +1 drive and two isolated catalogue
    // frames during the same bounded backward scan as the physical failure.
    auto disc = [&](int at,QFrame& q) {
        int lba=at+1, abs=lba+150, remain=3589-lba;
        if(lba<3364 || lba>3591) return false;
        q={};
        if(lba==3414 || lba==3514) q[0]=0x42;
        else {
            bool audio=lba<3365;
            q[0]=audio?1:0x41; q[1]=audio?1:2; q[2]=audio || lba>=3590;
            if(!q[2]) {q[4]=mixed::bcd(remain/75); q[5]=mixed::bcd(remain%75);}
            q[7]=mixed::bcd(abs/4500); q[8]=mixed::bcd(abs/75%60);
        }
        q[9]=mixed::bcd(abs%75); seal(q); return true;
    };
    int catalogs=0; offset=0;
    auto check = [&](int at,int tr,int ix,bool data) {
        QMatch result=mixed::matchQ(at,tr,ix,data,offset,disc);
        if(result==QMatch::Catalogue) ++catalogs;
        return result==QMatch::Catalogue || result==QMatch::Position;
    };
    CHECK(mixed::verifiedGap(check,3366,1,{0,0},{3590,1})==225 && catalogs==2);
    // Recorded T28 raw P-W: wrong INDEX bit, but the on-disc CRC is intact.
    QFrame prev{{1,0x28,0,0,2,0x22,0,0x48,0x26,0x24,0xa5,0x64}};
    QFrame next{{1,0x28,0,0,2,0x20,0,0x48,0x26,0x26,0xc1,0xa5}};
    QFrame raw{{1,0x28,0x20,0,2,0x21,0,0x48,0x26,0x25,0x5b,0x97}},fixed;
    CHECK(mixed::gapQFromNeighbours(raw,prev,next,217825,28,fixed));
    CHECK(mixed::position(fixed.data(),217825,28,0,false));
    raw[1]^=1; CHECK(!mixed::gapQFromNeighbours(raw,prev,next,217825,28,fixed));
    raw[1]^=1; raw[11]^=1; CHECK(!mixed::gapQFromNeighbours(raw,prev,next,217825,28,fixed));
    std::vector<QFrame> stream;
    for(int at=3364;at<=3590;++at) {
        QFrame frame; CHECK(disc(at-1,frame));
        if(at>=3365 && at<3440) { frame[0]&=0xbf; seal(frame); }
        stream.push_back(frame);
    }
    int firstData=-1;
    CHECK(mixed::streamConfirmsGap(stream,3365,1,&firstData) && firstData==3440);
    auto& finalGap=stream[stream.size()-2];
    finalGap={}; finalGap[0]=0x42; finalGap[9]=0x64; seal(finalGap);
    CHECK(mixed::streamConfirmsGap(stream,3365,1));
    finalGap[0]=2; seal(finalGap); CHECK(!mixed::streamConfirmsGap(stream,3365,1));
    finalGap[0]=0x42; seal(finalGap);
    stream[3]={}; CHECK(!mixed::streamConfirmsGap(stream,3365,1));

    // Last Armageddon: the three damaged raw frames repeat between reads.
    // Their on-disc CRC and the adjacent positions confirm the replacements.
    const QFrame l{{1,2,0,0,2,0x23,0,0,0x47,0x40,0xe7,0x64}};
    const QFrame r{{1,2,0,0,2,0x21,0,0,0x47,0x42,0x83,0xa5}};
    const QFrame damaged{{1,0x0a,0,0,2,0x22,0,0,0x47,0x41,0x5d,0x14}};
    std::vector<QFrame> evidence(3), decoded{l,l,r}, rawFrames{l,damaged,r};
    int repairs=0;
    CHECK(mixed::mergeGapFrames(evidence,decoded,{},3415,2,repairs));
    CHECK(!evidence[1][0]);
    CHECK(mixed::mergeGapFrames(evidence,rawFrames,{},3415,2,repairs) && repairs==0);
    CHECK(mixed::mergeGapFrames(evidence,rawFrames,rawFrames,3415,2,repairs) && repairs==1);
    CHECK(mixed::position(evidence[1].data(),3416,2,0,false));
    auto contradicted=rawFrames;
    contradicted[0][1]=3; seal(contradicted[0]);
    CHECK(!mixed::mergeGapFrames(evidence,contradicted,{},3415,2,repairs));
}
void pceCompatibilityTests() {
    const std::vector<mixed::Track> tracks{{0,0},{12,1,3,1},{24,0,0,2}};
    const auto original=mixed::cue("disc.img",tracks);
    auto expected=original;
    const std::string removed="    INDEX 00 00:00:08\r\n";
    expected.erase(expected.find(removed),removed.size());
    CHECK(mixed::cue("disc.img",tracks,true)==expected);
    CHECK(expected.find("INDEX 00 00:00:19")!=std::string::npos);

    FakeDrive drive;
    mixed::Sector sector;
    CHECK(drive.read(13,1,24,sector));
    const std::string marker="PC Engine CD-ROM SYSTEM";
    std::copy(marker.begin(),marker.end(),sector.begin()+48);
    std::string image(11*sector.size(),'\0');
    image.replace(10*sector.size(),sector.size(),reinterpret_cast<const char*>(sector.data()),sector.size());
    std::istringstream valid(image);
    CHECK(mixed::isPceCd(valid,tracks,30));
    auto differentMode=tracks;
    differentMode[1].mode=2;
    CHECK(!mixed::isPceCd(valid,differentMode,30));
    image[10*sector.size()+48]^=1;
    std::istringstream otherSystem(image);
    CHECK(!mixed::isPceCd(otherSystem,tracks,30));

    CHECK(drive.read(16,1,24,sector));
    const std::string gamesExpress="HACKER CD ROM SYSTEM";
    std::copy(gamesExpress.begin(),gamesExpress.end(),sector.begin()+24);
    image.assign(17*sector.size(),'\0');
    image.replace(16*sector.size(),sector.size(),reinterpret_cast<const char*>(sector.data()),sector.size());
    std::istringstream alternate(image);
    CHECK(mixed::isPceCd(alternate,{{0,1},{24,0}},30));
    CHECK(!mixed::isPceCd(alternate,{{0,1},{16,0}},30));
}
void tests() {
    // A final MODE1 read error may be filled without changing disc geometry.
    for (bool enabled : {false,true}) {
        FakeDrive drive;
        drive.allowFinalFill=enabled;
        drive.unreadable=15;
        std::vector<mixed::Track> tracks{{0,0},{12,1,4}};
        CHECK(mixed::acquire(tracks,16,true,drive,false)==enabled);
        CHECK(drive.filledLba==(enabled ? 15 : -1));
        if (enabled) CHECK(drive.kept.size()==12 && drive.kept.back()==0);
    }
    for (int unreadable : {12,14,15}) {
        FakeDrive drive;
        drive.allowFinalFill=true;
        drive.unreadable=unreadable;
        drive.dataReadError=unreadable!=15;
        std::vector<mixed::Track> tracks{{0,0},{12,1,4}};
        CHECK(!mixed::acquire(tracks,16,true,drive,false) && drive.filledLba==-1);
    }
    catalogGapTests();
    pceCompatibilityTests();
    using mixed::Track;
    std::vector<Track> initial{{0,0},{12,-1}}, tracks = initial;
    FakeDrive d;
    CHECK(mixed::acquire(tracks,16,false,d));
    CHECK(d.kept==std::vector<int>({0,1,2,3,4,5,6,7,12,13,14,15}));
    CHECK(tracks[1].omitted==4 && tracks[1].mode==1);
    auto preflightTracks=initial; FakeDrive preflight; preflight.preconfirmed=4;
    CHECK(mixed::acquire(preflightTracks,16,false,preflight));
    CHECK(preflight.kept==std::vector<int>({0,1,2,3,4,5,6,7,8,12,13,14,15}) && preflightTracks[1].omitted==3);
    // A drive returns real-looking bytes even after Q has switched to data.
    for (int readable : {8,9,10,12}) {
        preflight=FakeDrive(); preflight.preconfirmed=4; preflight.unreadable=-1;
        preflight.firstData=10; preflight.silentUntil=10; preflight.audioEnd=readable;
        preflightTracks=initial;
        CHECK(mixed::acquire(preflightTracks,16,false,preflight));
        CHECK(preflight.kept==d.kept && preflightTracks[1].omitted==4);
        CHECK(preflight.probes.back()<10);
    }
    // Genuine audio following leading silence is retained with its timing.
    preflight=FakeDrive(); preflight.preconfirmed=4; preflight.unreadable=-1;
    preflight.silentUntil=9; preflight.firstData=10; preflight.audioEnd=12;
    preflightTracks=initial;
    CHECK(mixed::acquire(preflightTracks,16,false,preflight));
    CHECK(preflight.kept==std::vector<int>({0,1,2,3,4,5,6,7,0,9,12,13,14,15}));
    CHECK(preflightTracks[1].omitted==2);
    preflightTracks=initial; preflight=FakeDrive(); preflight.preconfirmed=4; preflight.unreadable=6;
    CHECK(!mixed::acquire(preflightTracks,16,false,preflight));
    preflightTracks=initial; preflight=FakeDrive(); preflight.preconfirmed=4; preflight.prefixAt=11;
    CHECK(mixed::acquire(preflightTracks,16,false,preflight));
    CHECK(preflight.kept==std::vector<int>({0,1,2,3,4,5,6,7,8,11,12,13,14,15}));
    CHECK(preflightTracks[1].omitted==2 && preflightTracks[1].storedGap==1);
    CHECK(mixed::cue("early.img",preflightTracks).find("PREGAP 00:00:02\r\n    INDEX 00 00:00:09\r\n    INDEX 01 00:00:10")!=std::string::npos);
    // Even readable nonzero CD-DA in INDEX 00 gets no tail probe in the simple build.
    for (bool reference : {false,true}) {
        FakeDrive simple;
        simple.preconfirmed=4;
        simple.unreadable=-1;
        simple.audioEnd=12;
        auto simpleTracks=initial;
        if (reference) simpleTracks[1].omitted=4;
        CHECK(mixed::acquire(simpleTracks,16,reference,simple,false));
        CHECK(simple.probes.empty() && simpleTracks[1].omitted==4 && simple.kept==d.kept);
    }
    // Last Armageddon has a verified data sector one frame before the TOC.
    preflightTracks=initial; preflight=FakeDrive(); preflight.preconfirmed=4; preflight.prefixAt=11;
    CHECK(mixed::acquire(preflightTracks,16,false,preflight,false));
    CHECK(preflight.probes.empty() && preflightTracks[1].omitted==3 && preflightTracks[1].storedGap==1);
    CHECK(preflight.kept==std::vector<int>({0,1,2,3,4,5,6,7,11,12,13,14,15}));
    // Cobra has two readable INDEX 01 sectors before the TOC address.
    preflightTracks=initial; preflight=FakeDrive();
    preflight.preconfirmed=4; preflight.prefixAt=10; preflight.prefixCount=2; preflight.firstData=9;
    CHECK(mixed::acquire(preflightTracks,16,false,preflight,false));
    CHECK(preflightTracks[1].omitted==2 && preflightTracks[1].storedGap==2);
    CHECK(preflight.kept==std::vector<int>({0,1,2,3,4,5,6,7,10,11,12,13,14,15}));
    CHECK(mixed::cue("cobra.img",preflightTracks).find("PREGAP 00:00:02\r\n    INDEX 00 00:00:08\r\n    INDEX 01 00:00:10")!=std::string::npos);
    preflightTracks=initial; preflight.writeFail=11;
    CHECK(!mixed::acquire(preflightTracks,16,false,preflight,false));
    tracks[1].omitted=4; // input reference omits the whole gap
    auto text=mixed::cue("a.bin",tracks);
    CHECK(text.find("PREGAP 00:00:04\r\n    INDEX 01 00:00:08")!=std::string::npos);
    std::string bin;
    auto parse=[&](const std::string& t) {
        tracks=initial; std::istringstream in(t); return mixed::reference(in,tracks,bin);
    };
    CHECK(parse(text) && bin=="a.bin");
    d=FakeDrive(); d.badQ=9;
    CHECK(mixed::acquire(tracks,16,true,d) && d.kept.size()==13 && tracks[1].omitted==3);
    CHECK(d.probes==std::vector<int>({8,9}));
    CHECK(mixed::cue("r3.img",tracks).find("PREGAP 00:00:03\r\n    INDEX 01 00:00:09")!=std::string::npos);
    for (int readable : {0,2,4}) {
        CHECK(parse(text)); d=FakeDrive(); d.unreadable=-1; d.audioEnd=8+readable;
        CHECK(mixed::acquire(tracks,16,true,d) && tracks[1].omitted==4-readable);
        CHECK(int(d.kept.size())==12+readable && d.probes.back()==std::min(8+readable,11));
    }
    CHECK(parse(text)); d=FakeDrive(); d.unreadable=-1; d.audioEnd=12; d.headerAt=9;
    CHECK(mixed::acquire(tracks,16,true,d) && tracks[1].omitted==3);
    CHECK(parse(text)); d=FakeDrive(); d.cancelAt=8; CHECK(!mixed::acquire(tracks,16,true,d));
    CHECK(parse(text)); d=FakeDrive(); d.writeFail=8; CHECK(!mixed::acquire(tracks,16,true,d));
    CHECK(!parse(text.substr(0,text.find("  TRACK 02"))));
    CHECK(!parse(text + "FILE \"b.bin\" BINARY\n"));
    CHECK(!parse(text + "garbage\n"));
    auto wrong=text; wrong.replace(wrong.find("MODE1/2352"),10,"AUDIO");
    CHECK(!parse(wrong));
    wrong=text; wrong.replace(wrong.rfind("00:00:08"),8,"00:00:07"); CHECK(!parse(wrong));
    d=FakeDrive(); d.unreadable=-1; tracks=initial;
    CHECK(mixed::acquire(tracks,16,false,d) && tracks[1].storedGap==4 && d.kept.size()==16);
    CHECK(mixed::cue("b.img",tracks).find("INDEX 00 00:00:08")!=std::string::npos);
    for (int bad : {7,8,9,11,12}) {
        d=FakeDrive(); d.badQ=bad; tracks=initial;
        CHECK(!mixed::acquire(tracks,16,false,d) && tracks[1].omitted==0);
    }
    for (int failed : {0,4,12,13,15}) {
        d=FakeDrive(); d.unreadable=failed; tracks=initial;
        CHECK(!mixed::acquire(tracks,16,false,d));
    }
    d=FakeDrive(); d.trimOK=false; d.audioEnd=8; tracks=initial; CHECK(!mixed::acquire(tracks,16,false,d));
    d=FakeDrive(); d.writeFail=3; tracks=initial; CHECK(!mixed::acquire(tracks,16,false,d));
    d=FakeDrive(); d.cancel=true; tracks=initial; CHECK(!mixed::acquire(tracks,16,false,d));
    // CRC independently computed for ADR 1, data T02 INDEX 00, absolute 00:02:09.
    unsigned char q[]={0x41,2,0,0,0,3,0,0,2,9,0x3d,0x5e};
    CHECK(mixed::position(q,9,2,0,true));
    // Positions of the Q bits in a 96-symbol raw P-W reply for the same frame.
    std::array<unsigned char,96> pw;
    pw.fill(0xbf); // All other channels high.
    for (int bit : {1,7,14,46,47,70,76,79,82,83,84,85,87,89,91,92,93,94})
        pw[bit] |= 0x40;
    auto decoded = mixed::unpackQ(pw.data());
    CHECK(std::equal(decoded.begin(),decoded.end(),q) && mixed::position(decoded.data(),9,2,0,true));
    CHECK(!mixed::position(decoded.data(),9,2,1,true) && !mixed::position(decoded.data(),9,2,0,false));
    pw[70] ^= 0x40;
    CHECK(!mixed::validPosition(mixed::unpackQ(pw.data()).data(),9));
    CHECK(!mixed::position(q,10,2,0,true) && !mixed::position(q,9,1,0,true));
    q[3]^=1; CHECK(!mixed::position(q,9,2,0,true));

    // Gradius II T02 INDEX 01 is 00:49:65. The r5 drive log showed
    // 00:49:66 and CRC 7271 for the query at 3590.
    const mixed::QFrame exact{{0x41,2,1,0,0,0,0,0,0x49,0x65,0xe8,0x43}};
    const mixed::QFrame ahead{{0x41,2,1,0,0,1,0,0,0x49,0x66,0x72,0x71}};
    const mixed::QFrame early{{0x41,2,1,0,0,0,0,0,0x49,0x64,0xf8,0x62}};
    const mixed::QFrame physicalToc{{0x41,2,1,0,0,1,0,0,0x49,0x65,0x42,0x12}};
    std::vector<mixed::QFrame> positions{early,physicalToc};
    auto at=[&](int lba) { return positions.at(lba-3589); };
    CHECK(mixed::earlyDataFrames(at,3588,3590,2)==1);
    positions.back()=exact;
    CHECK(mixed::earlyDataFrames(at,3588,3590,2)==0);
    positions={exact,ahead};
    CHECK(mixed::earlyDataFrames(at,3588,3590,2)==0);

    // Recorded, CRC-checked Cobra Q frames at LBA 72737, 72738 and 72739.
    positions={{0x41,0x12,1,0,0,0,0,0x16,0x11,0x62,0xbd,0x8f},
               {0x41,0x12,1,0,0,1,0,0x16,0x11,0x63,0x07,0xff},
               {0x41,0x12,1,0,0,2,0,0x16,0x11,0x64,0x99,0xca}};
    auto cobraAt=[&](int lba) { return positions.at(lba-72737); };
    CHECK(mixed::earlyDataFrames(cobraAt,72512,72739,12)==2);
    CHECK(mixed::earlyDataFrames(cobraAt,72737,72739,12)==0);
    positions[1]=positions[0]; // A repeated Q frame cannot prove continuity.
    CHECK(mixed::earlyDataFrames(cobraAt,72512,72739,12)==0);
    mixed::QFrame received{};
    int offset = 0;
    std::vector<int> queries;
    auto shifted = [&](int at,mixed::QFrame& frame) {
        queries.push_back(at);
        frame = at==3589 ? exact : ahead;
        return true;
    };
    CHECK(mixed::readAlignedQ(3590,offset,received,shifted) && received==exact && offset==-1);
    CHECK(queries==std::vector<int>({3590,3589}));
    CHECK(!mixed::position(received.data(),3590,2,0,true));
    queries.clear();
    CHECK(mixed::readAlignedQ(3591,offset,received,shifted) && received==ahead);
    CHECK(queries==std::vector<int>({3590}));
    auto unshifted = [&](int at,mixed::QFrame& frame) { frame=exact; return at==3590; };
    CHECK(mixed::readAlignedQ(3590,offset,received,unshifted) && offset==0);
    auto repeated = [&](int,mixed::QFrame& frame) { frame=ahead; return true; };
    CHECK(!mixed::readAlignedQ(3590,offset,received,repeated) && offset==0);
    auto corrupt = [&](int at,mixed::QFrame& frame) { shifted(at,frame); frame[10]^=1; return true; };
    queries.clear();
    CHECK(!mixed::readAlignedQ(3590,offset,received,corrupt) && queries==std::vector<int>({3590}));
    auto badRetry = [&](int at,mixed::QFrame& frame) { shifted(at,frame); if(at==3589) frame[10]^=1; return true; };
    CHECK(!mixed::readAlignedQ(3590,offset,received,badRetry) && offset==0);
}

// An offline replay uses the old BIN as a stand-in for readable physical
// sectors. The three virtual ranges have no backing bytes and reject reads.
struct Replay : FakeDrive {
    std::ifstream disc,expected;
    std::ifstream tails;
    std::ofstream output;
    int count = 0, extra = 0;
    bool pendingTail = false;
    bool readAudioTail(int lba,int,int,mixed::Sector& s) {
        if (!tails.is_open() || (lba!=3365 && lba!=217772)) return false;
        if (!tails.read((char*)s.data(),s.size())) return false;
        pendingTail=true;
        return true;
    }
    explicit Replay(const char* file) : disc(file,std::ios::binary),expected(file,std::ios::binary) {}
    bool read(int lba,int,int,mixed::Sector& s) {
        int omitted=0;
        for (auto gap : {std::pair<int,int>{3365,3590},{6990,7140},{217772,217997}}) {
            if (lba>=gap.first && lba<gap.second) return false;
            if (lba>=gap.second) omitted+=gap.second-gap.first;
        }
        if (lba-omitted!=count) return false;
        return bool(disc.read((char*)s.data(),s.size()));
    }
    bool write(const mixed::Sector& s) {
        if (output.is_open() && !output.write((const char*)s.data(),s.size())) return false;
        if (pendingTail) { pendingTail=false; ++extra; return true; }
        mixed::Sector other;
        ++count;
        return expected.read((char*)other.data(),other.size()) && s==other;
    }
};
void replay(const char* cue,const char* image,const char* tails=nullptr,const char* folder=nullptr) {
    const int starts[]={0,3590,7140,9518,16866,22157,34057,38875,51235,65181,76726,92532,
        97350,102504,117099,122922,130125,146577,156130,160948,168026,175369,185463,
        199396,199957,207366,210189,217997};
    std::vector<mixed::Track> tracks;
    for (int s:starts) tracks.push_back({s,s==3590 || s==217997 ? -1 : 0});
    std::ifstream in(cue);
    std::string bin;
    CHECK(mixed::reference(in,tracks,bin));
    Replay drive(image);
    if (folder) {
        CHECK(_mkdir(folder)==0); // never replace an earlier replay
        drive.output.open(std::string(folder)+"/replay.img",std::ios::binary);
        drive.tails.open(tails,std::ios::binary|std::ios::ate);
        CHECK(drive.output && (drive.tails.tellg()==2352 || drive.tails.tellg()==4704));
        drive.tails.seekg(0);
    }
    CHECK(mixed::acquire(tracks,221248,true,drive));
    CHECK(drive.count==220648 && drive.expected.peek()==EOF);
    if (folder) {
        CHECK(drive.extra>=1 && drive.tails.peek()==EOF);
        drive.output.close(); CHECK(drive.output);
        std::ofstream out(std::string(folder)+"/replay.cue",std::ios::binary);
        out<<mixed::cue("replay.img",tracks); out.close(); CHECK(out);
    }
    std::cout << drive.count << " x 2352 bytes matched\n";
}
int main(int argc,char** argv) {
    try {
        tests();
        if (argc==3) replay(argv[1],argv[2]);
        else if (argc==5) replay(argv[1],argv[2],argv[3],argv[4]);
        else CHECK(argc==1);
        std::cout<<"OK\n";
    } catch (const std::exception& e) {
        std::cerr<<e.what()<<"\n"; return 1;
    }
}
