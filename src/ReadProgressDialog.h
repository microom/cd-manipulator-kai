#pragma once

#include "CDController.h"
#include "LogWindow.h"
#include "readthread.h"
#include "afxcmn.h"
#include "afxwin.h"

// CReadProgressDialog ダイアログ

class CReadProgressDialog : public CDialog
{
	DECLARE_DYNAMIC(CReadProgressDialog)

public:
	CReadProgressDialog(CWnd* pParent = NULL);   // 標準コンストラクタ
	virtual ~CReadProgressDialog();

// ダイアログ データ
	enum { IDD = IDD_READPROGRESS };

protected:
	virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV サポート
	CReadThread m_Thread;
	bool m_ShowTrackProgress = false;
	DWORD m_ReadStarted = 0;
	UINT_PTR m_ElapsedTimer = 0;
	void UpdateElapsedTime();
	afx_msg void OnTimer(UINT_PTR timerId);

	DECLARE_MESSAGE_MAP()
public:
	afx_msg void OnBnClickedOk();
	void ReadDisc(CCDController * cd, CLogWindow * logWnd, LPCSTR FileName,bool verify=false,bool createChd=false);
	void ReadMixed(CCDController *cd,CLogWindow *log,LPCSTR output,LPCSTR layout,bool createChd=false);
	void ReadTrack(CCDController * cd, CLogWindow * logWnd, LPCSTR FileName);
	virtual BOOL OnInitDialog();
	CString m_Percent;
	CProgressCtrl m_Progress;
	CString m_Message;
	CString m_CompletionDetail;
	afx_msg void OnBnClickedCancel();
	CButton m_CancelButton;
	afx_msg void OnBnClickedLog();
	CString m_Multi;
	afx_msg void OnWindowClose();
	bool GetSuccessFlag(void);
	bool m_NoConfirm;
	bool m_Stopped;
	afx_msg void OnUpdateDialog();
	void SetTrackProgress(int current,int total);
	afx_msg LRESULT OnTrackProgress(WPARAM current,LPARAM total);
};
