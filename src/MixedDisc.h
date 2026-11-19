#pragma once

#include <algorithm>
#include <array>
#include <cstdio>
#include <istream>
#include <sstream>
#include <string>
#include <vector>

namespace mixed {
using Sector = std::array<unsigned char, 2352>;
constexpr int maxVerifiedGap = 1500;
struct Track {
    int start;          // Drive LBA of INDEX 01
    int mode;           // 0 audio, -1 data (mode unknown), 1 or 2 data
    int omitted = 0;    // Frames before this track absent from the output image
    int storedGap = 0;  // Confirmed INDEX 00 present in the image
};
struct AudioGap {
    int frames = 0;     // From the start of INDEX 00 through the TOC boundary
    int firstData = -1; // First data-flagged LBA in the verified INDEX 00
    bool hasDataPrefix = false;
    Sector dataPrefix{};
};

inline bool msf(const std::string& s, int& frames) {
    if (s.size() != 8 || s[2] != ':' || s[5] != ':') return false;
    for (int i : {0, 1, 3, 4, 6, 7})
        if (s[i] < '0' || s[i] > '9') return false;
    int m = 10*(s[0]-'0') + s[1]-'0', sec = 10*(s[3]-'0') + s[4]-'0';
    int f = 10*(s[6]-'0') + s[7]-'0';
    frames = (60*m + sec)*75 + f;
    return sec < 60 && f < 75;
}

// Accept only a single BINARY file with explicit virtual PREGAPs.
// Each INDEX 01 is independently checked against the live disc TOC.
inline bool reference(std::istream& in, std::vector<Track>& tracks, std::string& bin) {
    std::vector<Track> candidate = tracks;
    std::string line, name;
    int n = -1, omitted = 0;
    bool indexed = false;
    while (std::getline(in, line)) {
        std::istringstream row(line);
        std::string key, a, b, extra;
        if (!(row >> key)) continue;
        if (key == "FILE") {
            if (!name.empty() || n != -1 || !(row >> std::ws) || row.get() != '"' ||
                !std::getline(row, name, '"') || name.empty() ||
                name.find_first_of("\\/:") != std::string::npos ||
                !(row >> a) || a != "BINARY") return false;
        } else if (key == "TRACK") {
            if (name.empty() || (n >= 0 && !indexed) || ++n >= int(candidate.size()) ||
                !(row >> a >> b) || std::to_string(n+1) !=
                (a.size() == 2 && a[0] == '0' ? a.substr(1) : a)) return false;
            int mode = b == "AUDIO" ? 0 : b == "MODE1/2352" ? 1 : b == "MODE2/2352" ? 2 : -2;
            if (mode < 0 || (mode == 0) != (candidate[n].mode == 0)) return false;
            candidate[n].mode = mode;
            indexed = false;
        } else if (key == "PREGAP") {
            int length;
            if (n < 1 || indexed || candidate[n].omitted || !(row >> a) ||
                !msf(a, length) || length <= 0 ||
                length >= candidate[n].start - candidate[n-1].start) return false;
            candidate[n].omitted = length;
            omitted += length;
        } else if (key == "INDEX") {
            int offset;
            if (n < 0 || indexed || !(row >> a >> b) || a != "01" ||
                !msf(b, offset) || offset + omitted != candidate[n].start) return false;
            indexed = true;
        } else return false;
        if (row >> extra) return false;
    }
    if (in.bad() || !indexed || n+1 != int(tracks.size())) return false;
    tracks.swap(candidate);
    bin = name;
    return true;
}

inline unsigned char bcd(int n) { return (n/10)*16 + n%10; }
inline bool address(const unsigned char* p, int lba) {
    int f = lba + 150;
    return p[0] == bcd(f/4500) && p[1] == bcd(f/75%60) && p[2] == bcd(f%75);
}
inline bool rawDataHeader(const Sector& s) {
    if (s[0] || s[11] || (s[15] != 1 && s[15] != 2)) return false;
    for (int i=1; i<=10; ++i) if (s[i] != 255) return false;
    return true;
}
inline bool dataHeader(const Sector& s, int lba) {
    return rawDataHeader(s) && address(s.data()+12,lba);
}

inline bool isPceCd(std::istream& image,const std::vector<Track>& tracks,int leadout) {
    int omitted=0;
    for (size_t i=0;i<tracks.size();++i) {
        const auto& track=tracks[i];
        omitted+=track.omitted;
        if (track.mode==0) continue;
        if (track.mode!=1) return false;
        const int end=i+1<tracks.size() ? tracks[i+1].start-tracks[i+1].omitted-tracks[i+1].storedGap : leadout;
        const auto matches=[&](int lba,int offset,const std::string& marker) {
            if (lba>=end || lba<omitted) return false;
            Sector sector;
            image.clear();
            image.seekg(std::streamoff(lba-omitted)*sector.size());
            return image.read(reinterpret_cast<char*>(sector.data()),sector.size()) &&
                   dataHeader(sector,lba) && sector[15]==1 &&
                   std::equal(marker.begin(),marker.end(),sector.begin()+offset);
        };
        // The offsets include the 16-byte raw Mode 1 header.
        return matches(track.start+1,48,"PC Engine CD-ROM SYSTEM") ||
               (i==0 && matches(track.start+16,24,"HACKER CD ROM SYSTEM"));
    }
    return false;
}
// Each of the 96 raw P-W symbols carries one Q bit (bit 6), MSB first.
inline std::array<unsigned char,12> unpackQ(const unsigned char* pw) {
    std::array<unsigned char,12> q{};
    for (int i=0; i<96; ++i)
        q[i/8] |= ((pw[i]>>6)&1) << (7-i%8);
    return q;
}
inline bool validQcrc(const unsigned char* q) {
    unsigned crc = 0;
    for (int i=0; i<10; ++i) {
        crc ^= unsigned(q[i]) << 8;
        for (int bit=0; bit<8; ++bit) crc = (crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0);
    }
    return ((~crc)&0xffff) == unsigned(q[10]*256 + q[11]);
}
inline bool validPosition(const unsigned char* q, int lba) {
    return (q[0]&15) == 1 && address(q+7,lba) && validQcrc(q);
}
inline bool position(const unsigned char* q, int lba, int track, int index, bool data) {
    return validPosition(q,lba) && bool(q[0]&0x40) == data &&
        q[1] == bcd(track) && q[2] == bcd(index);
}

// Some drives return Q for the frame after the requested main sector.
// Re-read the preceding address only after a CRC-checked +1 observation.
// The caller receives Q only for the target address, even at a track change.
using QFrame = std::array<unsigned char,12>;
template<class Read>
bool readAlignedQ(int target, int& requestOffset, QFrame& q, Read&& read) {
    const bool triedPrevious = requestOffset == -1 && target > 0;
    if (triedPrevious && read(target-1,q) && validPosition(q.data(),target)) return true;
    if (!read(target,q)) return false;
    if (validPosition(q.data(),target)) {
        requestOffset = 0;
        return true;
    }
    if (triedPrevious || target == 0 || !validPosition(q.data(),target+1)) return false;
    if (!read(target-1,q) || !validPosition(q.data(),target)) return false;
    requestOffset = -1;
    return true;
}

// A mode-2 catalogue frame has an absolute frame digit, but no track/index.
// Only an isolated frame inside INDEX 00 may use adjacent position reports.
inline int relativeTime(const QFrame& q) {
    int values[3];
    for (int i=0;i<3;++i) {
        int hi=q[3+i]>>4, lo=q[3+i]&15;
        if (hi>9 || lo>9) return -1;
        values[i]=10*hi+lo;
    }
    if (values[1]>=60 || values[2]>=75) return -1;
    return (values[0]*60+values[1])*75+values[2];
}
enum class QMatch { Unknown, Different, Position, Catalogue };
template<class Read>
QMatch matchQ(int target,int track,int index,bool data,int& offset,Read&& read) {
    int catalogAt = -1;
    unsigned char control = 0;
    auto observe = [&](int at,QFrame& q) {
        if (!read(at,q)) return false;
        if (index==0 && (q[0]&15)==2 && bool(q[0]&0x40)==data &&
            q[9]==bcd((target+150)%75) && validQcrc(q.data())) {
            catalogAt=at;
            control=q[0]&0xf0;
        }
        return true;
    };
    QFrame frame;
    if (readAlignedQ(target,offset,frame,observe))
        return position(frame.data(),target,track,index,data) ? QMatch::Position : QMatch::Different;
    if (catalogAt<1 || target<1) return QMatch::Unknown;
    QFrame before,after;
    if (!read(catalogAt-1,before) || !read(catalogAt+1,after) ||
        !position(before.data(),target-1,track,0,data) ||
        !position(after.data(),target+1,track,0,data) ||
        (before[0]&0xf0)!=control || (after[0]&0xf0)!=control)
        return QMatch::Unknown;
    const int remaining=relativeTime(after);
    return remaining>=0 && relativeTime(before)==remaining+2 ? QMatch::Catalogue : QMatch::Unknown;
}

// Every omitted sector needs evidence for the next track's INDEX 00.
// At an audio -> data transition a drive may stream Q across a range that it
// cannot seek into. Work only with independently CRC-checked, addressed slots.
inline bool addressedStreamQ(const QFrame& q,int lba) {
    return validPosition(q.data(),lba) ||
        ((q[0]&15)==2 && q[9]==bcd((lba+150)%75) && validQcrc(q.data()));
}
// Only repair evidence about an interior gap position. Keep the recorded
// CRC, and require the two measured position neighbours to predict all ten
// payload bytes to within exactly one bit of the raw subchannel.
inline bool gapQFromNeighbours(const QFrame& raw,const QFrame& left,const QFrame& right,
                                int at,int track,QFrame& repaired) {
    bool data=bool(left[0]&0x40);
    if (validQcrc(raw.data()) || left[0]!=right[0] ||
        !position(left.data(),at-1,track,0,data) || !position(right.data(),at+1,track,0,data) ||
        left[6]!=0 || right[6]!=0) return false;
    int last=relativeTime(right);
    if (last<0 || relativeTime(left)!=last+2) return false;
    QFrame q=left;
    int rel=last+1, absolute=at+150;
    q[3]=bcd(rel/4500); q[4]=bcd(rel/75%60); q[5]=bcd(rel%75);
    q[7]=bcd(absolute/4500); q[8]=bcd(absolute/75%60); q[9]=bcd(absolute%75);
    q[10]=raw[10]; q[11]=raw[11];
    int differences=0;
    for (int i=0;i<10;++i)
        for (unsigned bits=q[i]^raw[i];bits;bits&=bits-1) ++differences;
    if (differences!=1 || !validQcrc(q.data())) return false;
    repaired=q;
    return true;
}
inline bool streamConfirmsGap(const std::vector<QFrame>& frames,int failed,int track,int* firstData=nullptr) {
    if (frames.size()<3 || frames.size()>maxVerifiedGap+2) return false;
    int start=failed-1, end=start+int(frames.size())-1;
    if (!position(frames.front().data(),start,track,1,false) ||
        !position(frames.back().data(),end,track+1,1,true) || relativeTime(frames.back())!=0) return false;
    const size_t lastGap=frames.size()-2;
    const bool terminalCatalogue=(frames[lastGap][0]&15)==2;
    if (terminalCatalogue && lastGap<2) return false;
    const int finalCount=terminalCatalogue ? relativeTime(frames[lastGap-1])-1 : relativeTime(frames[lastGap]);
    if (finalCount!=0 && finalCount!=1) return false;
    int boundary=-1;
    for (int i=1;i<int(frames.size())-1;++i) {
        const QFrame& q=frames[i];
        int lba=start+i;
        bool data=(q[0]&0x40)!=0;
        if (boundary>=0 && !data) return false;
        if (data && boundary<0) boundary=lba;
        if ((q[0]&15)==2) {
            if (!addressedStreamQ(q,lba) ||
                !position(frames[i-1].data(),lba-1,track+1,0,data)) return false;
            if (size_t(i)==lastGap) {
                // An addressed catalogue frame may end the gap. Its data flag
                // and both adjoining positions must agree at the transition.
                if (!data || (q[0]&0xf0)!=(frames[i-1][0]&0xf0) ||
                    (q[0]&0xf0)!=(frames[i+1][0]&0xf0)) return false;
            } else if (!position(frames[i+1].data(),lba+1,track+1,0,data)) return false;
        } else if (!position(q.data(),lba,track+1,0,data) ||
                   relativeTime(q)!=end-1-lba+finalCount) return false;
    }
    if (boundary<0) return false;
    if (firstData) *firstData=boundary;
    return true;
}

inline bool indexOneFrameEarly(const QFrame& before,const QFrame& at,int toc,int track) {
    return position(before.data(),toc-1,track,1,true) && relativeTime(before)==0 &&
           position(at.data(),toc,track,1,true) && relativeTime(at)==1;
}

inline bool mergeGapFrames(std::vector<QFrame>& evidence,const std::vector<QFrame>& frames,
                           const std::vector<QFrame>& previous,int start,int track,int& repaired) {
    for (size_t i=0;i<frames.size();++i) {
        if (!addressedStreamQ(frames[i],start+int(i))) continue;
        if (evidence[i][0] && evidence[i]!=frames[i]) return false;
        evidence[i]=frames[i];
    }
    if (previous.empty()) return true;
    for (size_t i=1;i+1<frames.size();++i) {
        if (!evidence[i][0] && previous[i]==frames[i] &&
            gapQFromNeighbours(frames[i],evidence[i-1],evidence[i+1],start+int(i),track,evidence[i]))
            ++repaired;
    }
    return true;
}

// Check both neighbours as well. Unsupported Q formats simply give no proof.
template<class Q>
int verifiedGap(Q&& q, int failed, int trackNo, const Track& current, const Track& next) {
    const int end = next.start;
    const int limit = maxVerifiedGap; // bounded diagnostics (20 seconds of disc time)
    if (failed <= current.start || failed >= end || end-failed > limit ||
        !q(end, trackNo+1, 1, next.mode != 0)) return 0;
    for (int lba=end-1; lba>=current.start && end-lba<=limit+1; --lba) {
        if (q(lba, trackNo+1, 0, next.mode != 0)) continue;
        if (lba >= failed || !q(lba, trackNo, 1, current.mode != 0)) return 0;
        return end-lba-1;
    }
    return 0;
}

inline int omittedCount(const std::vector<Track>& tracks) {
    int total = 0;
    for (const auto& t : tracks) total += t.omitted;
    return total;
}
inline std::string time(int f) {
    char s[16];
    std::snprintf(s, sizeof(s), "%02d:%02d:%02d", f/4500, f/75%60, f%75);
    return s;
}
inline std::string cue(const std::string& image,const std::vector<Track>& tracks,bool pceCompatibility=false) {
    std::ostringstream out;
    out << "FILE \"" << image << "\" BINARY\r\n";
    int removed = 0, number = 0;
    for (const auto& t : tracks) {
        ++number;
        out << "  TRACK " << (number<10 ? "0" : "") << number << " "
            << (t.mode == 0 ? "AUDIO" : t.mode == 1 ? "MODE1/2352" : "MODE2/2352") << "\r\n";
        if (t.omitted) out << "    PREGAP " << time(t.omitted) << "\r\n";
        removed += t.omitted;
        if (t.storedGap && !(pceCompatibility && t.omitted))
            out << "    INDEX 00 " << time(t.start-removed-t.storedGap) << "\r\n";
        out << "    INDEX 01 " << time(t.start-removed) << "\r\n";
    }
    return out.str();
}

template<class IO>
bool acquire(std::vector<Track>& tracks, int leadout, bool hasReference, IO& io, bool recoverAudioTail=true) {
    int written = 0;
    Sector sector;
    for (int i=0; i<int(tracks.size()); ++i) {
        Track& t = tracks[i];
        Track* next = i+1<int(tracks.size()) ? &tracks[i+1] : nullptr;
        int end = next ? next->start-next->omitted : leadout;
        const bool audioToData = next && t.mode == 0 && next->mode != 0;
        const bool dataToAudio = next && t.mode != 0 && next->mode == 0;
        AudioGap audioGap;
        if (audioToData && !hasReference) {
            const int nextEnd=i+2<int(tracks.size()) ? tracks[i+2].start : leadout;
            audioGap=io.findAudioGap(t.start,end,nextEnd,i+1);
            if (audioGap.frames>0) {
                next->omitted=audioGap.frames;
                end-=audioGap.frames;
            }
        }
        const bool streamed=audioGap.frames>0;
        io.begin(i+1, t.start, end);
        // INDEX 01 in the TOC can follow a change of physical sector type.
        // Keep the unverified transition in single-sector reads.
        const int batchEnd=(dataToAudio || (audioToData && !hasReference && !streamed))
            ? end-maxVerifiedGap : end;
        for (int lba=t.start; lba<end; ++lba) {
            if (io.stopped()) return false;
            if (!io.read(lba, t.mode, batchEnd, sector)) {
                int gap = 0;
                auto query = [&](int at,int tr,int idx,bool data){ return io.q(at,tr,idx,data); };
                if (next && !hasReference && !streamed) {
                    if (audioToData && lba>t.start && end-lba<=maxVerifiedGap &&
                        io.streamGap(t.start,lba,end,i+1)) gap=end-lba;
                    else gap = verifiedGap(query, lba, i+1, t, *next);
                }
                if (!gap) return io.fail(i+1, lba, "sector read failed");
                // verifiedGap requires every INDEX 00 frame to have the
                // next track's control flag. A CD-DA reply cannot override it.
                const int discard = lba - (end-gap);
                if (discard && !io.trim(written-discard)) return io.fail(i+1,lba,"image truncate failed");
                written -= discard;
                next->omitted = gap;
                if (audioToData) io.tail(i+1,end-gap,0);
                io.gap(i+2, end-next->omitted, end, false);
                break;
            }
            if (lba == t.start && t.mode != 0) {
                if (!dataHeader(sector,lba) || (t.mode>0 && t.mode!=sector[15]))
                    return io.fail(i+1,lba,"data INDEX 01 header / mode mismatch");
                t.mode = sector[15];
            }
            if (!io.write(sector)) return io.fail(i+1,lba,"image write failed");
            ++written;
            io.progress(lba,leadout);
        }
        if (next && (hasReference || streamed) && next->omitted) {
            int at = end;
            const int gapEnd=next->start-(audioGap.hasDataPrefix ? 1 : 0);
            if (audioToData && recoverAudioTail) {
                const int audioEnd=streamed ? audioGap.firstData : gapEnd;
                if (audioEnd<end || audioEnd>gapEnd) return io.fail(i+1,end,"invalid audio/data boundary");
                const Sector silence{};
                for (int lba=end; lba<audioEnd; ++lba) {
                    if (io.stopped()) return false;
                    if (!io.readAudioTail(lba,t.start,audioEnd,sector) || rawDataHeader(sector)) break;
                    // Represent a wholly silent prefix by PREGAP. If audio
                    // follows, write the leading silence at its original length.
                    if (streamed && at==end && sector==silence) continue;
                    while (at<lba) {
                        if (!io.write(silence)) return io.fail(i+1,at,"audio tail write failed");
                        ++at;
                        ++written;
                    }
                    if (!io.write(sector)) return io.fail(i+1,at,"audio tail write failed");
                    ++at;
                    ++written;
                }
                if (io.stopped()) return false;
                io.tail(i+1,end,at-end);
            }
            next->omitted=gapEnd-at;
            if (next->omitted) io.gap(i+2,at,gapEnd,hasReference);
            if (audioGap.hasDataPrefix) {
                if (!io.write(audioGap.dataPrefix)) return io.fail(i+2,gapEnd,"data prefix write failed");
                ++written;
                next->storedGap=1;
            }
        }
        if (next && !hasReference && !streamed && !next->omitted && !io.stopped()) {
            auto query = [&](int at,int tr,int idx,bool data){ return io.q(at,tr,idx,data); };
            next->storedGap = verifiedGap(query, end-1, i+1, t, *next);
        }
    }
    return !io.stopped() && written == leadout-omittedCount(tracks);
}
}
