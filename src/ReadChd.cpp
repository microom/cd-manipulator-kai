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

ChdResult ConvertCue(const CStringW& executable, const CStringW& cue, const CStringW& output,
                     const std::string& replacement,const bool& stopped)
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
    STARTUPINFOW startup{};
    startup.cb=sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL started=CreateProcessW(executable,command.GetBuffer(),nullptr,nullptr,FALSE,
                                      CREATE_NO_WINDOW,nullptr,directory,&startup,&process);
    command.ReleaseBuffer();

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

    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!stopped && complete && GetFileAttributesExW(temporary.path,GetFileExInfoStandard,&data) &&
        (data.nFileSizeLow || data.nFileSizeHigh) && MoveFileW(temporary.path,output)) {
        temporary.path.Empty();
        return ChdResult::Saved;
    }
    if (stopped) return ChdResult::Cancelled;
    if (GetFileAttributesW(output)!=INVALID_FILE_ATTRIBUTES) return ChdResult::Skipped;
    return ChdResult::Failed;
}
}

void CReadThread::CreateChd()
{
    auto& dlg=*static_cast<CReadProgressDialog*>(m_ParentWnd);
    const bool japanese=theSetting.m_LangFile.Right(12).CompareNoCase("Japanese.cml")==0;
    dlg.m_Message=CString(japanese ? L"CHD \u3092\u751f\u6210\u4e2d..." : L"Creating CHD...");
    dlg.PostMessage(WM_COMMAND,ID_UPDATE_DIALOG);
    m_LogWnd->AddMessage(LOG_INFO,dlg.m_Message);

    const CStringW cue(m_ChdCue);
    wchar_t path[MAX_PATH];
    const DWORD length=GetFullPathNameW(CStringW(m_CuePath),MAX_PATH,path,nullptr);
    CStringW output;
    ChdResult result=ChdResult::Failed;
    if (length && length<MAX_PATH && m_CuePath.Right(4).CompareNoCase(".cue")==0) {
        output=path;
        output=output.Left(output.GetLength()-4)+L".chd";
        wchar_t input[MAX_PATH];
        const DWORD inputLength=cue.IsEmpty() ? 0 : GetFullPathNameW(cue,MAX_PATH,input,nullptr);
        if (inputLength && inputLength<MAX_PATH) {
            result=ConvertCue(theSetting.AvailableChdman(),input,output,m_ChdCueText,m_StopFlag);
        }
    }

    CStringW message;
    int severity=LOG_WARNING;
    switch (result) {
    case ChdResult::Saved:
        message=japanese ? L"CHD \u3092\u4fdd\u5b58\u3057\u307e\u3057\u305f\u3002" : L"CHD saved.";
        severity=LOG_NORMAL;
        if (!m_ChdCueText.empty())
            m_LogWnd->AddMessage(LOG_INFO,"PCE-CD CUE compatibility: skipped INDEX 00 on tracks with explicit PREGAP for CHD conversion.");
        break;
    case ChdResult::Skipped:
        message=japanese ? L"\u540c\u540d\u306e CHD \u304c\u65e2\u306b\u3042\u308a\u307e\u3059\u3002"
                         : L"A CHD with this name already exists.";
        break;
    case ChdResult::Cancelled:
        message=japanese ? L"CHD \u306e\u751f\u6210\u3092\u4e2d\u65ad\u3057\u307e\u3057\u305f\u3002"
                         : L"CHD creation cancelled.";
        break;
    case ChdResult::Failed:
        message=japanese ? L"CHD \u306e\u751f\u6210\u306b\u5931\u6557\u3057\u307e\u3057\u305f\u3002"
                         : L"CHD creation failed.";
        break;
    }
    dlg.m_CompletionDetail=CString(message);
    if (!output.IsEmpty()) message+=L" "+output;
    m_LogWnd->AddMessage(severity,CString(message));
}
