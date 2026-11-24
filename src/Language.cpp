#include "stdafx.h"
#include "language.h"
#include "resource.h"

namespace {
const char* const KaiKeys[] = {
	"AutoRead", "MixedSkipTail", "MixedPreserveTail", "MixedReference", "MixedNote",
	"AutoCorrectAudio", "CreateChd", "DeleteChdSource", "ZeroFillFinal", "Elapsed",
	"AudioPregapFilled", "FinalSectorFilled", "ChdCreating", "ChdSaved",
	"ChdSourceDeleted", "ChdSourceDeleteFailed", "ChdExists", "ChdCancelled", "ChdFailed"
};
static_assert(_countof(KaiKeys) == static_cast<int>(KaiText::Count), "Kai language keys must match KaiText");
}

CLanguage::CLanguage(void)
{
}

CLanguage::~CLanguage(void)
{
}

const CString& CLanguage::Kai(KaiText text) const
{
	return m_KaiText[static_cast<int>(text)];
}

void CLanguage::ReadKaiLine(LPCSTR line)
{
	constexpr char prefix[] = "#Kai.";
	if (strncmp(line, prefix, sizeof(prefix) - 1) != 0) return;
	const char* key = line + sizeof(prefix) - 1;
	const char* value = strchr(key, '=');
	if (!value || !value[1]) return;
	for (int i = 0; i < static_cast<int>(KaiText::Count); ++i) {
		if (strlen(KaiKeys[i]) == static_cast<size_t>(value - key) &&
		    strncmp(KaiKeys[i], key, value - key) == 0) {
			m_KaiText[i] = value + 1;
			return;
		}
	}
}

void CLanguage::LoadKaiDefaults()
{
	for (auto& text : m_KaiText) text.Empty();

	const HMODULE module = GetModuleHandle(nullptr);
	const HRSRC resource = FindResource(module, MAKEINTRESOURCE(IDR_KAI_ENGLISH), RT_RCDATA);
	if (!resource) return;
	const HGLOBAL data = LoadResource(module, resource);
	const auto* bytes = static_cast<const char*>(data ? LockResource(data) : nullptr);
	if (!bytes) return;

	CString dictionary(bytes, static_cast<int>(SizeofResource(module, resource)));
	int position = 0;
	while (position >= 0) {
		CString line = dictionary.Tokenize("\n", position);
		line.TrimRight('\r');
		ReadKaiLine(line);
	}
}

void CLanguage::ReadLanguage(LPCSTR FileName)
{
	int i;
	char Buffer[1024];
	FILE *fp;

	fp = fopen(FileName,"r");
	if(fp == NULL){
		sprintf(Buffer,"Can't read language file\nFile:'%s'",FileName);
		MessageBox(NULL,Buffer,"Caution!!",MB_OK);
		return;
	}
	LoadKaiDefaults();
	i = 0;
	while(!feof(fp)){
		Buffer[0] = '\0';
		fgets(Buffer,1023,fp);
		if(Buffer[0] == '\0'){
			break;
		}
		Buffer[1023] = '\0';
		if(Buffer[lstrlen(Buffer) - 1] == '\n'){
			Buffer[lstrlen(Buffer) - 1] = '\0';
		}
		if(Buffer[0] == '#'){
			ReadKaiLine(Buffer);
		} else if(i < STRING_COUNT){
			m_Str[i].Format("%s",Buffer);
			i++;
		}
	}
	fclose(fp);

/*	{
		CString cs;
		cs.Format("String count:%d",i);
		MessageBox(NULL,cs,"Assertion!!",MB_OK);
	}
// */
}
