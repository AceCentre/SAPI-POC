#pragma once

#include <atlbase.h>
#include <atlcom.h>
#include <rpc.h>
#include <sphelper.h>
#include <spcollec.h>
#include <spddkhlp.h>

#include "pysapittsengine.h"
#include "resource.h"
#include "pycpp.h"

class ATL_NO_VTABLE Engine : 
	public CComObjectRootEx<CComMultiThreadModel>,
	public CComCoClass<Engine, &CLSID_PySAPITTSEngine>,
	public ISpTTSEngine,
    public ISpObjectWithToken
{
public:
    DECLARE_REGISTRY_RESOURCEID(IDR_PYSAPITTSENGINE)
    DECLARE_PROTECT_FINAL_CONSTRUCT()

    BEGIN_COM_MAP(Engine)
	    COM_INTERFACE_ENTRY(ISpTTSEngine)
	    COM_INTERFACE_ENTRY(ISpObjectWithToken)
    END_COM_MAP()

    HRESULT FinalConstruct();
    void FinalRelease();

    // ISpObjectWithToken
    HRESULT __stdcall SetObjectToken(ISpObjectToken * pToken);
    HRESULT __stdcall GetObjectToken(ISpObjectToken ** ppToken);

    // ISpTTSEngine
    HRESULT __stdcall Speak(DWORD dwSpeakFlags, REFGUID rguidFormatId, const WAVEFORMATEX * pWaveFormatEx,
                            const SPVTEXTFRAG* pTextFragList, ISpTTSEngineSite* pOutputSite);
    HRESULT __stdcall GetOutputFormat(const GUID * pTargetFormatId, const WAVEFORMATEX * pTargetWaveFormatEx,
                                      GUID * pDesiredFormatId, WAVEFORMATEX ** ppCoMemDesiredWaveFormatEx);

private:
    CComPtr<ISpObjectToken> token_;
    pycpp::PythonVM vm_;

    // TTS methods
    pycpp::Obj speak_method_;

    int handle_actions(ISpTTSEngineSite* site);
};
