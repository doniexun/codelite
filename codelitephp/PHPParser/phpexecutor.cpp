#include "phpexecutor.h"
#include <globals.h>
#include <wx/tokenzr.h>
#include <processreaderthread.h>
#include <wx/app.h>
#include "clplatform.h"
#include "php_project_settings_data.h"
#include "environmentconfig.h"
#include "file_logger.h"
#include <wx/msgdlg.h>
#include "php_workspace.h"
#include <dirsaver.h>
#include <wx/process.h>
#include <cl_config.h>
#include "php_configuration_data.h"
#include "php_event.h"
#include <event_notifier.h>
#include <wx/uri.h>
#include "asyncprocess.h"

PHPExecutor::PHPExecutor()
    : m_process(NULL)
{
}

PHPExecutor::~PHPExecutor() {}

bool PHPExecutor::Exec(const wxString& projectName, const wxString& xdebugSessionName, bool neverPauseOnExit)
{
    PHPProject::Ptr_t proj = PHPWorkspace::Get()->GetProject(projectName);
    CHECK_PTR_RET_FALSE(proj);

    if(proj->GetSettings().GetRunAs() == PHPProjectSettingsData::kRunAsWebsite) {
        // Execute the URL
        if(proj->GetSettings().GetProjectURL().IsEmpty()) {
            ::wxMessageBox(_("Empty URL is provided"), wxT("CodeLite"), wxOK | wxICON_ERROR, wxTheApp->GetTopWindow());
            return false;
        }
        return RunRUL(proj, xdebugSessionName);

    } else {
        return DoRunCLI("", proj, xdebugSessionName, neverPauseOnExit);
    }
}

bool PHPExecutor::IsRunning() const { return m_process != NULL; }

void PHPExecutor::OnProcessTerminated(wxProcessEvent& e) { wxDELETE(m_process); }

void PHPExecutor::Stop()
{
    if(IsRunning()) {
        wxProcess::Kill(m_process->GetPid(), wxSIGTERM, wxKILL_CHILDREN);
        wxDELETE(m_process);
    }
}

bool PHPExecutor::RunRUL(PHPProject::Ptr_t pProject, const wxString& xdebugSessionName)
{
    const PHPProjectSettingsData& data = pProject->GetSettings();
    wxURI uri(data.GetProjectURL());

    wxString url;
    wxString queryStrnig = uri.GetQuery();
    if(queryStrnig.IsEmpty() && !xdebugSessionName.IsEmpty()) {
        // no query string was provided by the user
        url << uri.BuildURI() << "?XDEBUG_SESSION_START=" << xdebugSessionName;

    } else {
        url << uri.BuildURI();
    }

    CL_DEBUG("CodeLite: Calling URL: " + url);
    PHPEvent evtLoadURL(wxEVT_PHP_LOAD_URL);
    evtLoadURL.SetUrl(url);
    evtLoadURL.SetUseDefaultBrowser(data.IsUseSystemBrowser());
    EventNotifier::Get()->AddPendingEvent(evtLoadURL);
    return true;
}

bool PHPExecutor::DoRunCLI(const wxString& script,
                           PHPProject::Ptr_t proj,
                           const wxString& xdebugSessionName,
                           bool neverPauseOnExit)
{
    if(IsRunning()) {
        ::wxMessageBox(_("Another process is already running"),
                       wxT("CodeLite"),
                       wxOK | wxICON_INFORMATION,
                       wxTheApp->GetTopWindow());
        return false;
    }

    wxString errmsg;
    wxString cmd = DoGetCLICommand(script, proj, errmsg);
    if(cmd.IsEmpty()) {
        ::wxMessageBox(errmsg, wxT("CodeLite"), wxOK | wxICON_INFORMATION, wxTheApp->GetTopWindow());
        return false;
    }
    bool pauseWhenTerminate(false);
    wxString wd;
    if(proj) {
        const PHPProjectSettingsData& data = proj->GetSettings();
        pauseWhenTerminate = data.IsPauseWhenExeTerminates();
        wd = data.GetWorkingDirectory();
    }

    cmd = ::MakeExecInShellCommand(cmd, wd, (pauseWhenTerminate && !neverPauseOnExit));
    wxLogMessage(cmd);

    // Apply the environment variables
    // export XDEBUG_CONFIG="idekey=session_name remote_host=localhost profiler_enable=1"
    wxStringMap_t om;
    if(!xdebugSessionName.IsEmpty()) {

        PHPConfigurationData phpGlobalSettings;
        phpGlobalSettings.Load();
        int port = phpGlobalSettings.GetXdebugPort();

        wxString envname = "XDEBUG_CONFIG";
        wxString envvalue;
        envvalue << "idekey=" << xdebugSessionName << " remote_port=" << port;
        om.insert(std::make_pair(envname, envvalue));
    }

    EnvSetter serrter(&om);
    DirSaver ds;
    ::wxSetWorkingDirectory(wd);

    // Execute the command
    m_process = new wxProcess(wxPROCESS_DEFAULT);
    if(::wxExecute(cmd, wxEXEC_SHOW_CONSOLE | wxEXEC_ASYNC, m_process) > 0) {
        if(m_process) {
            m_process->Connect(wxEVT_END_PROCESS, wxProcessEventHandler(PHPExecutor::OnProcessTerminated), NULL, this);
        }
    } else {
        wxDELETE(m_process);
    }
    return m_process != NULL;
}

bool PHPExecutor::RunScript(const wxString& script, wxString& php_output)
{
    wxString errmsg;
    wxString cmd = DoGetCLICommand(script, PHPProject::Ptr_t(NULL), errmsg);
    if(cmd.IsEmpty()) {
        ::wxMessageBox(errmsg, wxT("CodeLite"), wxOK | wxICON_INFORMATION, wxTheApp->GetTopWindow());
        return false;
    }

    IProcess::Ptr_t phpcli(::CreateSyncProcess(cmd, IProcessCreateDefault | IProcessCreateWithHiddenConsole));
    CHECK_PTR_RET_FALSE(phpcli);
    
    phpcli->WaitForTerminate(php_output);
    return true;
}

wxString PHPExecutor::DoGetCLICommand(const wxString& script, PHPProject::Ptr_t proj, wxString& errmsg)
{
    wxArrayString args;
    wxString php;
    wxArrayString includePath;
    wxString index;
    wxString ini;

    PHPConfigurationData globalConf;
    globalConf.Load();

    if(proj) {
        const PHPProjectSettingsData& data = proj->GetSettings();
        args = ::wxStringTokenize(data.GetArgs(), wxT("\r"), wxTOKEN_STRTOK);
        includePath = data.GetIncludePathAsArray();
        php = data.GetPhpExe();
        index = data.GetIndexFile();
        ini = data.GetPhpIniFile();

    } else {

        index = script;
        php = globalConf.GetPhpExe();
        includePath = globalConf.GetIncludePaths();
    }

    ini.Trim().Trim(false);
    if(ini.Contains(" ")) {
        ini.Prepend("\"").Append("\"");
    }
    if(index.IsEmpty()) {
        errmsg = _("Please set an index file to execute in the project settings");
        return "";
    }

    if(php.IsEmpty()) {
        php = globalConf.GetPhpExe();
        if(php.IsEmpty()) {
            errmsg = _("Could not find any PHP binary to execute. Please set one in from: 'PHP | Settings'");
            return "";
        }
    }

    // An example of execution:
    // php.exe -c /path/to/ini.ini -d "display_errors=On" -d include_path="C:\php\includes" file2.php

    // Build the command for execution
    wxString cmd;

    cmd << php;              // Wrap the php exe with qoutes
    cmd.Replace(" ", "\\ "); // escape spaces
    if(!ini.IsEmpty()) {
        cmd << " -c " << ini << " ";
    }

    cmd << wxT(" -d display_errors=On "); // Enable error reporting
    cmd << wxT(" -d html_errors=Off ");

    // add the include path
    if(includePath.IsEmpty() == false) {
        cmd << wxT("-d include_path=\"");
        for(size_t i = 0; i < includePath.GetCount(); i++) {
            cmd << includePath.Item(i) << clPlatform::PathSeparator;
        }
        cmd << wxT("\" ");
    }

    cmd << index;

    if(!args.IsEmpty()) {
        cmd << " ";
    }

    // set the program arguments to run (after the index file is set)
    for(size_t i = 0; i < args.GetCount(); ++i) {
        cmd << args.Item(i) << " ";
    }
    return cmd;
}
