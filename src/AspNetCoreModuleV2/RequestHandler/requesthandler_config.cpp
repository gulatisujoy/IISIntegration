// Copyright (c) .NET Foundation. All rights reserved.
// Licensed under the MIT License. See License.txt in the project root for license information.

#include "stdafx.h"
#include "requesthandler_config.h"
#include "debugutil.h"

REQUESTHANDLER_CONFIG::~REQUESTHANDLER_CONFIG()
{
    if (m_ppStrArguments != NULL)
    {
        delete[] m_ppStrArguments;
        m_ppStrArguments = NULL;
    }

    if (m_pEnvironmentVariables != NULL)
    {
        m_pEnvironmentVariables->Clear();
        delete m_pEnvironmentVariables;
        m_pEnvironmentVariables = NULL;
    }
}

HRESULT
REQUESTHANDLER_CONFIG::CreateRequestHandlerConfig(
    _In_  IHttpServer             *pHttpServer,
    _In_  IHttpApplication        *pHttpApplication,
    _In_  PCWSTR                   pwzExeLocation,
    _In_  HANDLE                   hEventLog,
    _Out_ REQUESTHANDLER_CONFIG  **ppAspNetCoreConfig
)
{
    HRESULT                 hr = S_OK;
    REQUESTHANDLER_CONFIG  *pRequestHandlerConfig = NULL;
    STRU                    struHostFxrDllLocation;
    BSTR*                   pwzArgv;
    DWORD                   dwArgCount;
    STRU                    struExeLocation;

    if (ppAspNetCoreConfig == NULL)
    {
        hr = E_INVALIDARG;
        goto Finished;
    }

    *ppAspNetCoreConfig = NULL;

    pRequestHandlerConfig = new REQUESTHANDLER_CONFIG;
    if (pRequestHandlerConfig == NULL)
    {
        hr = E_OUTOFMEMORY;
        goto Finished;
    }

    hr = pRequestHandlerConfig->Populate(pHttpServer, pHttpApplication);
    if (FAILED(hr))
    {
        goto Finished;
    }

    // Modify config for inprocess.
    if (pRequestHandlerConfig->QueryHostingModel() == APP_HOSTING_MODEL::HOSTING_IN_PROCESS)
    {
        if (FAILED(struExeLocation.Copy(pwzExeLocation)))
        {
            goto Finished;
        }
        // If the exe was not provided by the shim, reobtain the hostfxr parameters (which finds dotnet).
        if (struExeLocation.IsEmpty())
        {
            if (FAILED(hr = HOSTFXR_UTILITY::GetHostFxrParameters(
                hEventLog,
                pRequestHandlerConfig->QueryProcessPath()->QueryStr(),
                pRequestHandlerConfig->QueryApplicationPhysicalPath()->QueryStr(),
                pRequestHandlerConfig->QueryArguments()->QueryStr(),
                &struHostFxrDllLocation,
                &struExeLocation,
                &dwArgCount,
                &pwzArgv)))
            {
                goto Finished;
            }
        }
        else if (HOSTFXR_UTILITY::IsDotnetExecutable(&struExeLocation))
        {
            if (FAILED(hr = HOSTFXR_UTILITY::ParseHostfxrArguments(
                pRequestHandlerConfig->QueryArguments()->QueryStr(),
                pwzExeLocation,
                pRequestHandlerConfig->QueryApplicationPhysicalPath()->QueryStr(),
                hEventLog,
                &dwArgCount,
                &pwzArgv)))
            {
                goto Finished;
            }
        }
        else
        {
            if (FAILED(hr = HOSTFXR_UTILITY::GetStandaloneHostfxrParameters(
                pwzExeLocation,
                pRequestHandlerConfig->QueryApplicationPhysicalPath()->QueryStr(),
                pRequestHandlerConfig->QueryArguments()->QueryStr(),
                hEventLog,
                &struHostFxrDllLocation,
                &dwArgCount,
                &pwzArgv)))
            {
                goto Finished;
            }
        }

        pRequestHandlerConfig->SetHostFxrArguments(dwArgCount, pwzArgv);
    }

    DebugPrintf(ASPNETCORE_DEBUG_FLAG_INFO,
        "REQUESTHANDLER_CONFIG::GetConfig, set config to ModuleContext");
    // set appliction info here instead of inside Populate()
    // as the destructor will delete the backend process
    hr = pRequestHandlerConfig->QueryApplicationPath()->Copy(pHttpApplication->GetApplicationId());
    if (FAILED(hr))
    {
        goto Finished;
    }

    *ppAspNetCoreConfig = pRequestHandlerConfig;
    pRequestHandlerConfig = NULL;

Finished:

    if (pRequestHandlerConfig != NULL)
    {
        delete pRequestHandlerConfig;
        pRequestHandlerConfig = NULL;
    }

    return hr;
}

HRESULT
REQUESTHANDLER_CONFIG::Populate(
    IHttpServer    *pHttpServer,
    IHttpApplication   *pHttpApplication
)
{
    STACK_STRU(strHostingModel, 300);
    HRESULT                         hr = S_OK;
    STRU                            strEnvName;
    STRU                            strEnvValue;
    STRU                            strExpandedEnvValue;
    STRU                            strApplicationFullPath;
    IAppHostAdminManager           *pAdminManager = NULL;
    IAppHostElement                *pAspNetCoreElement = NULL;
    IAppHostElement                *pWindowsAuthenticationElement = NULL;
    IAppHostElement                *pBasicAuthenticationElement = NULL;
    IAppHostElement                *pAnonymousAuthenticationElement = NULL;
    IAppHostElement                *pEnvVarList = NULL;
    IAppHostElement                *pEnvVar = NULL;
    IAppHostElementCollection      *pEnvVarCollection = NULL;
    ULONGLONG                       ullRawTimeSpan = 0;
    ENUM_INDEX                      index;
    ENVIRONMENT_VAR_ENTRY*          pEntry = NULL;
    DWORD                           dwCounter = 0;
    DWORD                           dwPosition = 0;
    WCHAR*                          pszPath = NULL;
    BSTR                            bstrWindowAuthSection = NULL;
    BSTR                            bstrBasicAuthSection = NULL;
    BSTR                            bstrAnonymousAuthSection = NULL;
    BSTR                            bstrAspNetCoreSection = NULL;

    m_pEnvironmentVariables = new ENVIRONMENT_VAR_HASH();
    if (m_pEnvironmentVariables == NULL)
    {
        hr = E_OUTOFMEMORY;
        goto Finished;
    }
    if (FAILED(hr = m_pEnvironmentVariables->Initialize(37 /*prime*/)))
    {
        delete m_pEnvironmentVariables;
        m_pEnvironmentVariables = NULL;
        goto Finished;
    }

    pAdminManager = pHttpServer->GetAdminManager();
    hr = m_struConfigPath.Copy(pHttpApplication->GetAppConfigPath());
    if (FAILED(hr))
    {
        goto Finished;
    }

    hr = m_struApplicationPhysicalPath.Copy(pHttpApplication->GetApplicationPhysicalPath());
    if (FAILED(hr))
    {
        goto Finished;
    }

    pszPath = m_struConfigPath.QueryStr();
    while (pszPath[dwPosition] != NULL)
    {
        if (pszPath[dwPosition] == '/')
        {
            dwCounter++;
            if (dwCounter == 4)
                break;
        }
        dwPosition++;
    }

    if (dwCounter == 4)
    {
        hr = m_struApplicationVirtualPath.Copy(pszPath + dwPosition);
    }
    else
    {
        hr = m_struApplicationVirtualPath.Copy(L"/");
    }

    // Will setup the application virtual path.
    if (FAILED(hr))
    {
        goto Finished;
    }

    bstrWindowAuthSection = SysAllocString(CS_WINDOWS_AUTHENTICATION_SECTION);
    if (bstrWindowAuthSection == NULL)
    {
        hr = E_OUTOFMEMORY;
        goto Finished;
    }

    hr = pAdminManager->GetAdminSection(bstrWindowAuthSection,
        m_struConfigPath.QueryStr(),
        &pWindowsAuthenticationElement);
    if (FAILED(hr))
    {
        // assume the corresponding authen was not enabled
        // as the section may get deleted by user in some HWC case
        // ToDo: log a warning to event log
        m_fWindowsAuthEnabled = FALSE;
    }
    else
    {
        hr = GetElementBoolProperty(pWindowsAuthenticationElement,
            CS_ENABLED,
            &m_fWindowsAuthEnabled);
        if (FAILED(hr))
        {
            goto Finished;
        }
    }

    bstrBasicAuthSection = SysAllocString(CS_BASIC_AUTHENTICATION_SECTION);
    if (bstrBasicAuthSection == NULL)
    {
        hr = E_OUTOFMEMORY;
        goto Finished;
    }
    hr = pAdminManager->GetAdminSection(bstrBasicAuthSection,
        m_struConfigPath.QueryStr(),
        &pBasicAuthenticationElement);
    if (FAILED(hr))
    {
        m_fBasicAuthEnabled = FALSE;
    }
    else
    {
        hr = GetElementBoolProperty(pBasicAuthenticationElement,
            CS_ENABLED,
            &m_fBasicAuthEnabled);
        if (FAILED(hr))
        {
            goto Finished;
        }
    }
    bstrAnonymousAuthSection = SysAllocString(CS_ANONYMOUS_AUTHENTICATION_SECTION);
    if (bstrAnonymousAuthSection == NULL)
    {
        hr = E_OUTOFMEMORY;
        goto Finished;
    }
    hr = pAdminManager->GetAdminSection(bstrAnonymousAuthSection,
        m_struConfigPath.QueryStr(),
        &pAnonymousAuthenticationElement);
    if (FAILED(hr))
    {
        m_fAnonymousAuthEnabled = FALSE;
    }
    else
    {
        hr = GetElementBoolProperty(pAnonymousAuthenticationElement,
            CS_ENABLED,
            &m_fAnonymousAuthEnabled);
        if (FAILED(hr))
        {
            goto Finished;
        }
    }

    bstrAspNetCoreSection = SysAllocString(CS_ASPNETCORE_SECTION);
    if (bstrAspNetCoreSection == NULL)
    {
        hr = E_OUTOFMEMORY;
        goto Finished;
    }
    hr = pAdminManager->GetAdminSection(bstrAspNetCoreSection,
        m_struConfigPath.QueryStr(),
        &pAspNetCoreElement);
    if (FAILED(hr))
    {
        goto Finished;
    }

    hr = GetElementStringProperty(pAspNetCoreElement,
        CS_ASPNETCORE_PROCESS_EXE_PATH,
        &m_struProcessPath);
    if (FAILED(hr))
    {
        goto Finished;
    }

    hr = GetElementStringProperty(pAspNetCoreElement,
        CS_ASPNETCORE_HOSTING_MODEL,
        &strHostingModel);
    if (FAILED(hr))
    {
        // Swallow this error for backward compatability
        // Use default behavior for empty string
        hr = S_OK;
    }

    if (strHostingModel.IsEmpty() || strHostingModel.Equals(L"outofprocess", TRUE))
    {
        m_hostingModel = HOSTING_OUT_PROCESS;
    }
    else if (strHostingModel.Equals(L"inprocess", TRUE))
    {
        m_hostingModel = HOSTING_IN_PROCESS;
    }
    else
    {
        // block unknown hosting value
        hr = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        goto Finished;
    }

    hr = GetElementStringProperty(pAspNetCoreElement,
        CS_ASPNETCORE_PROCESS_ARGUMENTS,
        &m_struArguments);
    if (FAILED(hr))
    {
        goto Finished;
    }

    hr = GetElementDWORDProperty(pAspNetCoreElement,
        CS_ASPNETCORE_RAPID_FAILS_PER_MINUTE,
        &m_dwRapidFailsPerMinute);
    if (FAILED(hr))
    {
        goto Finished;
    }

    //
    // rapidFailsPerMinute cannot be greater than 100.
    //
    if (m_dwRapidFailsPerMinute > MAX_RAPID_FAILS_PER_MINUTE)
    {
        m_dwRapidFailsPerMinute = MAX_RAPID_FAILS_PER_MINUTE;
    }

    hr = GetElementDWORDProperty(pAspNetCoreElement,
        CS_ASPNETCORE_PROCESSES_PER_APPLICATION,
        &m_dwProcessesPerApplication);
    if (FAILED(hr))
    {
        goto Finished;
    }

    hr = GetElementDWORDProperty(
        pAspNetCoreElement,
        CS_ASPNETCORE_PROCESS_STARTUP_TIME_LIMIT,
        &m_dwStartupTimeLimitInMS
    );
    if (FAILED(hr))
    {
        goto Finished;
    }

    m_dwStartupTimeLimitInMS *= MILLISECONDS_IN_ONE_SECOND;

    hr = GetElementDWORDProperty(
        pAspNetCoreElement,
        CS_ASPNETCORE_PROCESS_SHUTDOWN_TIME_LIMIT,
        &m_dwShutdownTimeLimitInMS
    );
    if (FAILED(hr))
    {
        goto Finished;
    }
    m_dwShutdownTimeLimitInMS *= MILLISECONDS_IN_ONE_SECOND;

    hr = GetElementBoolProperty(pAspNetCoreElement,
        CS_ASPNETCORE_FORWARD_WINDOWS_AUTH_TOKEN,
        &m_fForwardWindowsAuthToken);
    if (FAILED(hr))
    {
        goto Finished;
    }

    hr = GetElementBoolProperty(pAspNetCoreElement,
        CS_ASPNETCORE_DISABLE_START_UP_ERROR_PAGE,
        &m_fDisableStartUpErrorPage);
    if (FAILED(hr))
    {
        goto Finished;
    }

    hr = GetElementRawTimeSpanProperty(
        pAspNetCoreElement,
        CS_ASPNETCORE_WINHTTP_REQUEST_TIMEOUT,
        &ullRawTimeSpan
    );
    if (FAILED(hr))
    {
        goto Finished;
    }

    m_dwRequestTimeoutInMS = (DWORD)TIMESPAN_IN_MILLISECONDS(ullRawTimeSpan);

    hr = GetElementBoolProperty(pAspNetCoreElement,
        CS_ASPNETCORE_STDOUT_LOG_ENABLED,
        &m_fStdoutLogEnabled);
    if (FAILED(hr))
    {
        goto Finished;
    }
    hr = GetElementStringProperty(pAspNetCoreElement,
        CS_ASPNETCORE_STDOUT_LOG_FILE,
        &m_struStdoutLogFile);
    if (FAILED(hr))
    {
        goto Finished;
    }

    hr = GetElementChildByName(pAspNetCoreElement,
        CS_ASPNETCORE_ENVIRONMENT_VARIABLES,
        &pEnvVarList);
    if (FAILED(hr))
    {
        goto Finished;
    }

    hr = pEnvVarList->get_Collection(&pEnvVarCollection);
    if (FAILED(hr))
    {
        goto Finished;
    }

    for (hr = FindFirstElement(pEnvVarCollection, &index, &pEnvVar);
        SUCCEEDED(hr);
        hr = FindNextElement(pEnvVarCollection, &index, &pEnvVar))
    {
        if (hr == S_FALSE)
        {
            hr = S_OK;
            break;
        }

        if (FAILED(hr = GetElementStringProperty(pEnvVar,
            CS_ASPNETCORE_ENVIRONMENT_VARIABLE_NAME,
            &strEnvName)) ||
            FAILED(hr = GetElementStringProperty(pEnvVar,
                CS_ASPNETCORE_ENVIRONMENT_VARIABLE_VALUE,
                &strEnvValue)) ||
            FAILED(hr = strEnvName.Append(L"=")) ||
            FAILED(hr = STRU::ExpandEnvironmentVariables(strEnvValue.QueryStr(), &strExpandedEnvValue)))
        {
            goto Finished;
        }

        pEntry = new ENVIRONMENT_VAR_ENTRY();
        if (pEntry == NULL)
        {
            hr = E_OUTOFMEMORY;
            goto Finished;
        }

        if (FAILED(hr = pEntry->Initialize(strEnvName.QueryStr(), strExpandedEnvValue.QueryStr())) ||
            FAILED(hr = m_pEnvironmentVariables->InsertRecord(pEntry)))
        {
            goto Finished;
        }
        strEnvName.Reset();
        strEnvValue.Reset();
        strExpandedEnvValue.Reset();
        pEnvVar->Release();
        pEnvVar = NULL;
        pEntry->Dereference();
        pEntry = NULL;
    }

Finished:

    if (pAspNetCoreElement != NULL)
    {
        pAspNetCoreElement->Release();
        pAspNetCoreElement = NULL;
    }

    if (pWindowsAuthenticationElement != NULL)
    {
        pWindowsAuthenticationElement->Release();
        pWindowsAuthenticationElement = NULL;
    }

    if (pAnonymousAuthenticationElement != NULL)
    {
        pAnonymousAuthenticationElement->Release();
        pAnonymousAuthenticationElement = NULL;
    }

    if (pBasicAuthenticationElement != NULL)
    {
        pBasicAuthenticationElement->Release();
        pBasicAuthenticationElement = NULL;
    }

    if (pEnvVarList != NULL)
    {
        pEnvVarList->Release();
        pEnvVarList = NULL;
    }

    if (pEnvVar != NULL)
    {
        pEnvVar->Release();
        pEnvVar = NULL;
    }

    if (pEnvVarCollection != NULL)
    {
        pEnvVarCollection->Release();
        pEnvVarCollection = NULL;
    }

    if (pEntry != NULL)
    {
        pEntry->Dereference();
        pEntry = NULL;
    }

    return hr;
}