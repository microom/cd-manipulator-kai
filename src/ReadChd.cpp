#include "stdafx.h"
#include "Resource.h"
#include "ReadProgressDialog.h"
#include "Setting.h"
#include <fstream>
#include <iterator>

namespace {
enum class ChdResult { Saved, Skipped, Failed, Cancelled };

struct TempFile {
    CStringW path;

    bool Create(const CStringW& directory, bool cue=false)
    {
        wchar_t temp[MAX_PATH];
        if (!GetTempFileNameW(directory,L"cdm",0,temp)) return false;
        path=temp;
        if (!cue) return true;
        const CStringW cuePath=path+L".cue";
        if (!MoveFileW(path,cuePath)) return false;
        path=cuePath;
        return true;
    }

    CStringW Name() const { return path.Mid(path.ReverseFind(L'\\')+1); }
    ~TempFile() { if (!path.IsEmpty()) DeleteFileW(path); }
};

bool WriteUtf8Cue(const CStringW& from,const CStringW& to,const std::string& replacement)
{
    std::string sourceText;
    if (replacement.empty()) {
        std::ifstream source(from.GetString(),std::ios::binary);
        if (!source) return false;
        sourceText.assign(std::istreambuf_iterator<char>(source),{});
        if (sourceText.empty() || source.bad()) return false;
    }
    const auto& contents=replacement.empty() ? sourceText : replacement;
    const CStringW wide(contents.c_str());
    const CW2A utf8(wide,CP_UTF8);
    std::ofstream dest(to.GetString(),std::ios::binary);
    dest << static_cast<const char*>(utf8);
    dest.close();
    return bool(dest);
}

bool RunChdman(const CStringW& executable,CStringW command,const CStringW& directory,
               const bool& stopped,HANDLE log=INVALID_HANDLE_VALUE)
{
    if (stopped) return false;
    STARTUPINFOW startup{};
    startup.cb=sizeof(startup);
    const bool capture=log!=INVALID_HANDLE_VALUE;
    HANDLE input=INVALID_HANDLE_VALUE;
    if (capture) {
        SECURITY_ATTRIBUTES inherit{sizeof(inherit),nullptr,TRUE};
        input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ | FILE_SHARE_WRITE,
                          &inherit,OPEN_EXISTING,0,nullptr);
        if (input==INVALID_HANDLE_VALUE) return false;
        startup.dwFlags=STARTF_USESTDHANDLES;
        startup.hStdInput=input;
        startup.hStdOutput=startup.hStdError=log;
    }
    PROCESS_INFORMATION process{};
    const BOOL started=CreateProcessW(executable,command.GetBuffer(),nullptr,nullptr,capture,
                                      CREATE_NO_WINDOW,nullptr,directory,&startup,&process);
    command.ReleaseBuffer();
    if (capture) CloseHandle(input);

    bool complete=false;
    if (started) {
        CloseHandle(process.hThread);
        DWORD status;
        do {
            status=WaitForSingleObject(process.hProcess,100);
        } while (status==WAIT_TIMEOUT && !stopped);
        if (status!=WAIT_OBJECT_0) {
            TerminateProcess(process.hProcess,1);
            WaitForSingleObject(process.hProcess,1000);
        } else {
            DWORD code;
            complete=GetExitCodeProcess(process.hProcess,&code) && code==0;
        }
        CloseHandle(process.hProcess);
    }

    return complete;
}

bool VerifyChd(const CStringW& executable,const CStringW& directory,const TempFile& image,
               const bool& stopped)
{
    TempFile report;
    if (!report.Create(directory)) return false;
    SECURITY_ATTRIBUTES inherit{sizeof(inherit),nullptr,TRUE};
    HANDLE log=CreateFileW(report.path,GENERIC_WRITE,FILE_SHARE_READ,&inherit,
                           TRUNCATE_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (log==INVALID_HANDLE_VALUE) return false;
    CStringW command;
    command.Format(L"chdman.exe verify -i \"%s\"",image.Name().GetString());
    const bool complete=RunChdman(executable,command,directory,stopped,log);
    CloseHandle(log);
    if (!complete) return false;

    // chdman 0.287 can print a checksum mismatch and exit with status zero.
    std::ifstream result(report.path.GetString(),std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(result)),{});
    return text.find("Raw SHA1 verification successful!")!=std::string::npos &&
           text.find("Overall SHA1 verification successful!")!=std::string::npos;
}

ChdResult ConvertCue(const CStringW& executable, const CStringW& cue, const CStringW& output,
                     const std::string& replacement,bool verify,const bool& stopped)
{
    if (stopped) return ChdResult::Cancelled;
    if (GetFileAttributesW(output)!=INVALID_FILE_ATTRIBUTES) return ChdResult::Skipped;
    if (executable.IsEmpty() || cue.IsEmpty()) return ChdResult::Failed;

    // chdman reads UTF-8 CUEs. Use ASCII relative arguments so it can open them in any directory.
    const CStringW directory=cue.Left(cue.ReverseFind(L'\\')+1);
    TempFile input,temporary;
    if (!input.Create(directory,true) || !WriteUtf8Cue(cue,input.path,replacement) || !temporary.Create(directory))
        return ChdResult::Failed;

    CStringW command;
    command.Format(L"chdman.exe createcd -i \"%s\" -o \"%s\" -f",
                   input.Name().GetString(),temporary.Name().GetString());
    const bool complete=RunChdman(executable,command,directory,stopped);

    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!stopped && complete && GetFileAttributesExW(temporary.path,GetFileExInfoStandard,&data) &&
        (data.nFileSizeLow || data.nFileSizeHigh)) {
        if (verify && !VerifyChd(executable,directory,temporary,stopped))
            return stopped ? ChdResult::Cancelled : ChdResult::Failed;
        if (!stopped && MoveFileW(temporary.path,output)) {
            temporary.path.Empty();
            return ChdResult::Saved;
        }
    }
    if (stopped) return ChdResult::Cancelled;
    if (GetFileAttributesW(output)!=INVALID_FILE_ATTRIBUTES) return ChdResult::Skipped;
    return ChdResult::Failed;
}
}

void CReadThread::CreateChd()
{
    auto& dlg=*static_cast<CReadProgressDialog*>(m_ParentWnd);
    const auto& lang=theSetting.m_Lang;
    dlg.m_Message=lang.Kai(KaiText::ChdCreating);
    dlg.PostMessage(WM_COMMAND,ID_UPDATE_DIALOG);
    m_LogWnd->AddMessage(LOG_INFO,dlg.m_Message);

    const CStringW cue(m_ChdCue);
    wchar_t path[MAX_PATH];
    const DWORD length=GetFullPathNameW(CStringW(m_CuePath),MAX_PATH,path,nullptr);
    CStringW output, sourceCue;
    ChdResult result=ChdResult::Failed;
    if (length && length<MAX_PATH && m_CuePath.Right(4).CompareNoCase(".cue")==0) {
        output=path;
        output=output.Left(output.GetLength()-4)+L".chd";
        wchar_t input[MAX_PATH];
        const DWORD inputLength=cue.IsEmpty() ? 0 : GetFullPathNameW(cue,MAX_PATH,input,nullptr);
        if (inputLength && inputLength<MAX_PATH) {
            sourceCue=input;
            result=ConvertCue(theSetting.AvailableChdman(),sourceCue,output,m_ChdCueText,
                              m_DeleteChdSource,m_StopFlag);
        }
    }

    CString message;
    int severity=LOG_WARNING;
    switch (result) {
    case ChdResult::Saved:
        message=lang.Kai(KaiText::ChdSaved);
        severity=LOG_NORMAL;
        if (!m_ChdCueText.empty())
            m_LogWnd->AddMessage(LOG_INFO,"PCE-CD CUE compatibility: skipped INDEX 00 on tracks with explicit PREGAP for CHD conversion.");
        if (m_DeleteChdSource && !m_StopFlag && sourceCue.Right(4).CompareNoCase(L".cue")==0) {
            // Each generated CUE uses a same-name IMG. If Redump produced a
            // verified copy, sourceCue refers to the pair in that folder.
            const CStringW image=sourceCue.Left(sourceCue.GetLength()-4)+L".img";
            if (DeleteFileW(image) && DeleteFileW(sourceCue)) {
                message+="\n"+lang.Kai(KaiText::ChdSourceDeleted);
            } else {
                severity=LOG_WARNING;
                message+="\n"+lang.Kai(KaiText::ChdSourceDeleteFailed);
                m_LogWnd->AddMessage(LOG_WARNING,CString(sourceCue));
            }
        }
        break;
    case ChdResult::Skipped:
        message=lang.Kai(KaiText::ChdExists);
        break;
    case ChdResult::Cancelled:
        message=lang.Kai(KaiText::ChdCancelled);
        break;
    case ChdResult::Failed:
        message=lang.Kai(KaiText::ChdFailed);
        break;
    }
    if (!dlg.m_CompletionDetail.IsEmpty()) dlg.m_CompletionDetail+="\n";
    dlg.m_CompletionDetail+=message;
    if (!output.IsEmpty()) message+=" "+CString(output);
    m_LogWnd->AddMessage(severity,message);
}
