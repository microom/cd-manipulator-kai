// ReadProgressDialog.cpp : 実装ファイル
//

#include "stdafx.h"
#include "CDM.h"
#include "ReadProgressDialog.h"
#include "Setting.h"
#include "Mmsystem.h"

namespace {
constexpr UINT TrackProgressMessage=WM_APP+1;
constexpr UINT_PTR ElapsedTimerId=1;
}

// CReadProgressDialog ダイアログ

IMPLEMENT_DYNAMIC(CReadProgressDialog, CDialog)
CReadProgressDialog::CReadProgressDialog(CWnd* pParent /*=NULL*/)
	: CDialog(CReadProgressDialog::IDD, pParent)
	, m_Percent(_T(""))
	, m_Message(_T(""))
	, m_Multi(_T(""))
{
	m_NoConfirm = false;
	m_Stopped = false;
}

CReadProgressDialog::~CReadProgressDialog()
{
}

void CReadProgressDialog::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	DDX_Text(pDX, IDC_PERCENT, m_Percent);
	DDX_Control(pDX, IDC_PROGRESS, m_Progress);
	DDX_Text(pDX, IDC_MESSAGE, m_Message);
	DDX_Control(pDX, IDCANCEL, m_CancelButton);
	if(!m_ShowTrackProgress) DDX_Text(pDX, IDC_MULTI, m_Multi);
}


BEGIN_MESSAGE_MAP(CReadProgressDialog, CDialog)
	ON_BN_CLICKED(IDOK, OnBnClickedOk)
	ON_BN_CLICKED(IDCANCEL, OnBnClickedCancel)
	ON_BN_CLICKED(IDC_LOG, OnBnClickedLog)
	ON_COMMAND(ID_WINDOW_CLOSE, OnWindowClose)
	ON_COMMAND(ID_UPDATE_DIALOG, OnUpdateDialog)
	ON_MESSAGE(TrackProgressMessage, OnTrackProgress)
	ON_WM_TIMER()
END_MESSAGE_MAP()


// CReadProgressDialog メッセージ ハンドラ

void CReadProgressDialog::OnBnClickedOk()
{
	// TODO : ここにコントロール通知ハンドラ コードを追加します。
}

void CReadProgressDialog::ReadDisc(CCDController * cd, CLogWindow * logWnd, LPCSTR FileName,bool verify,bool createChd)
{
	int ReadAudioMethod;
	CString DriveName,Message;
	CString Address;

	cd->GetDriveName(DriveName);
	Message.Format(MSG(96),DriveName);
	logWnd->AddMessage(LOG_NORMAL,Message);

	m_Thread.m_CD = cd;
	m_Thread.m_LogWnd = logWnd;
	m_Thread.m_FileName = FileName;
	m_Thread.m_ParentWnd = this;
	cd->GetAspiCtrl()->GetDeviceString(m_Thread.m_VendorName,m_Thread.m_ProductName,m_Thread.m_Revision,Address);

	m_Thread.m_ReadImage = true;
	m_Thread.m_VerifyAfterRead = verify;
	m_Thread.m_CreateChdAfterRead = createChd;
	m_Thread.m_DeleteChdSource = createChd && theSetting.m_DeleteChdSource;
	ReadAudioMethod = theSetting.m_ReadAudioMethod;

	DoModal();

	theSetting.m_ReadAudioMethod = ReadAudioMethod;
	cd->SetSpeed(0xff,0xff);

	m_Thread.StopThread();
}

void CReadProgressDialog::ReadMixed(CCDController *cd,CLogWindow *log,LPCSTR output,LPCSTR layout,bool createChd)
{
	m_Thread.m_MixedDisc = true;
	m_Thread.m_LayoutFile = layout;
	ReadDisc(cd,log,output,false,createChd);
}

void CReadProgressDialog::ReadTrack(CCDController * cd, CLogWindow * logWnd, LPCSTR FileName)
{
	int ReadAudioMethod;
	CString DriveName,Message;

	cd->GetDriveName(DriveName);
	Message.Format(MSG(96),DriveName);
	logWnd->AddMessage(LOG_NORMAL,Message);

	m_Thread.m_CD = cd;
	m_Thread.m_LogWnd = logWnd;
	m_Thread.m_FileName = FileName;
	m_Thread.m_ParentWnd = this;

	m_Thread.m_ReadImage = false;
	ReadAudioMethod = theSetting.m_ReadAudioMethod;

	DoModal();

	theSetting.m_ReadAudioMethod = ReadAudioMethod;
	cd->SetSpeed(0xff,0xff);

	m_Thread.StopThread();
}

BOOL CReadProgressDialog::OnInitDialog()
{
	m_ShowTrackProgress = false;
	CDialog::OnInitDialog();

	// TODO :  ここに初期化を追加してください
	m_ReadStarted=GetTickCount();
	UpdateElapsedTime();
	m_ElapsedTimer=SetTimer(ElapsedTimerId,1000,nullptr);
	m_Thread.StartThread();

	SetWindowText(theSetting.m_Lang.m_Str[LP_READP + 0]);
	this->SetDlgItemText(IDC_LOG,theSetting.m_Lang.m_Str[LP_READP + 1]);
	this->SetDlgItemText(IDCANCEL,theSetting.m_Lang.m_Str[LP_READP + 2]);
	m_Stopped = false;

	return TRUE;  // return TRUE unless you set the focus to a control
	// 例外 : OCX プロパティ ページは必ず FALSE を返します。
}

void CReadProgressDialog::OnBnClickedCancel()
{
	// TODO : ここにコントロール通知ハンドラ コードを追加します。
	m_Thread.m_LogWnd->AddMessage(LOG_WARNING,MSG(17));
	if(m_Thread.m_MixedDisc){
		m_Thread.m_StopFlag = true;
		m_Stopped = true;
		m_CancelButton.EnableWindow(FALSE);
	} else if(m_Thread.m_StopFlag){
		OnCancel();
	} else {
		m_Thread.m_StopFlag = true;
		m_Stopped = true;
	}
}

void CReadProgressDialog::OnBnClickedLog()
{
	// TODO : ここにコントロール通知ハンドラ コードを追加します。
	if(m_Thread.m_LogWnd->GetStyle() & 0x10000000){
		m_Thread.m_LogWnd->ShowWindow(SW_HIDE);
	} else {
		m_Thread.m_LogWnd->ShowWindow(SW_SHOW);
	}
}

void CReadProgressDialog::OnWindowClose()
{
	// TODO : ここにコマンド ハンドラ コードを追加します。
	m_Thread.StopThread();
	if (m_ElapsedTimer) {
		KillTimer(m_ElapsedTimer);
		m_ElapsedTimer=0;
		UpdateElapsedTime();
	}

	if(m_Stopped){
	} else if(!m_Thread.m_Success){
		CString cs;
		cs.Format("%s\n%s",m_Message,MSG(18));
		if(theSetting.m_WavOnFail != ""){
			PlaySound(theSetting.m_WavOnFail,NULL,SND_ASYNC);
		}
		MessageBox(cs,CONF_MSG,MB_OK);
	} else if(!m_NoConfirm){
		if(theSetting.m_WavOnSuccess != ""){
			PlaySound(theSetting.m_WavOnSuccess,NULL,SND_ASYNC);
		}
		CString message=MSG(19);
		if(!m_CompletionDetail.IsEmpty()) message+="\n"+m_CompletionDetail;
		MessageBox(message,CONF_MSG,MB_OK);
	}

	OnCancel();
}

bool CReadProgressDialog::GetSuccessFlag(void)
{
	return m_Thread.m_Success;
}

void CReadProgressDialog::UpdateElapsedTime()
{
	const DWORD seconds=(GetTickCount()-m_ReadStarted)/1000;
	CString elapsed;
	elapsed.Format("%s %02lu:%02lu:%02lu",theSetting.m_Lang.Kai(KaiText::Elapsed).GetString(),
	               seconds/3600,(seconds/60)%60,seconds%60);
	SetDlgItemText(IDC_READ_ELAPSED,elapsed);
}

void CReadProgressDialog::OnTimer(UINT_PTR timerId)
{
	if (timerId==m_ElapsedTimer)
		UpdateElapsedTime();
	else
		CDialog::OnTimer(timerId);
}

void CReadProgressDialog::OnUpdateDialog()
{
	// TODO : ここにコマンド ハンドラ コードを追加します。
	UpdateData(FALSE);
}

void CReadProgressDialog::SetTrackProgress(int current,int total)
{
	PostMessage(TrackProgressMessage,current,total);
}

LRESULT CReadProgressDialog::OnTrackProgress(WPARAM current,LPARAM total)
{
	CString progress;
	progress.Format("%d / %d",static_cast<int>(current),static_cast<int>(total));
	m_ShowTrackProgress = true;
	SetDlgItemText(IDC_MULTI,progress);
	return 0;
}
