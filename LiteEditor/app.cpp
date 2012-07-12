////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////
//
// copyright            : (C) 2008 by Eran Ifrah
// file name            : app.cpp
//
// -------------------------------------------------------------------------
// A
//              _____           _      _     _ _
//             /  __ \         | |    | |   (_) |
//             | /  \/ ___   __| | ___| |    _| |_ ___
//             | |    / _ \ / _  |/ _ \ |   | | __/ _ )
//             | \__/\ (_) | (_| |  __/ |___| | ||  __/
//              \____/\___/ \__,_|\___\_____/_|\__\___|
//
//                                                  F i l e
//
//    This program is free software; you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation; either version 2 of the License, or
//    (at your option) any later version.
//
//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////

#include "precompiled_header.h"
#include "cl_registry.h"
#include "file_logger.h"
#include "fileextmanager.h"
#include "evnvarlist.h"
#include "environmentconfig.h"
#include "conffilelocator.h"
#include "app.h"
#include "dirsaver.h"
#include "xmlutils.h"
#include "editor_config.h"
#include "wx_xml_compatibility.h"
#include "manager.h"
#include "exelocator.h"
#include "macros.h"
#include "stack_walker.h"
#include "procutils.h"
#include "globals.h"
#include "frame.h"
#include "buidltab.h"
#include "asyncprocess.h" // IProcess

#define __PERFORMANCE
#include "performance.h"

//////////////////////////////////////////////
// Define the version string for this codelite
//////////////////////////////////////////////
extern wxChar *SvnRevision;
wxString CODELITE_VERSION_STR = wxString::Format(wxT("v4.0.%s"), SvnRevision);

#if defined(__WXMAC__)||defined(__WXGTK__)
#include <sys/wait.h>
#include <signal.h> // sigprocmask
#endif

#ifdef __WXMSW__
#include <wx/msw/registry.h> //registry keys
void EnableDebugPriv()
{
    HANDLE hToken;
    LUID luid;
    TOKEN_PRIVILEGES tkp;

    OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken);

    LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &luid);

    tkp.PrivilegeCount = 1;
    tkp.Privileges[0].Luid = luid;
    tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    AdjustTokenPrivileges(hToken, false, &tkp, sizeof(tkp), NULL, NULL);

    CloseHandle(hToken);
}
#endif

#ifdef __WXMAC__
#include <mach-o/dyld.h>

//On Mac we determine the base path using system call
//_NSGetExecutablePath(path, &path_len);
wxString MacGetBasePath()
{
	char path[257];
	uint32_t path_len = 256;
	_NSGetExecutablePath(path, &path_len);

	//path now contains
	//CodeLite.app/Contents/MacOS/
	wxFileName fname(wxString(path, wxConvUTF8));

	//remove he MacOS part of the exe path
	wxString file_name = fname.GetPath(wxPATH_GET_VOLUME|wxPATH_GET_SEPARATOR);
	wxString rest;
	file_name.EndsWith(wxT("MacOS/"), &rest);
	rest.Append(wxT("SharedSupport/"));

	return rest;
}
#endif
//-------------------------------------------------
// helper method to draw the revision + version
// on our splash screen
//-------------------------------------------------
static wxBitmap clDrawSplashBitmap(const wxBitmap& bitmap, const wxString &mainTitle)
{
	wxBitmap bmp ( bitmap.GetWidth(), bitmap.GetHeight()  );
	wxMemoryDC dcMem;

	dcMem.SelectObject( bmp );
	dcMem.DrawBitmap  ( bitmap, 0, 0, true);

	//write the main title & subtitle
	wxCoord w, h;
	wxFont font = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
	wxFont smallfont = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
	font.SetPointSize(12);
	smallfont.SetPointSize(10);
	dcMem.SetFont(font);
	dcMem.GetMultiLineTextExtent(mainTitle, &w, &h);
	wxCoord bmpW = bitmap.GetWidth();

	//draw shadow
	dcMem.SetTextForeground(wxT("WHITE"));

	wxCoord textX = (bmpW - w)/2;
	dcMem.DrawText(mainTitle, textX, 11);
	dcMem.SelectObject(wxNullBitmap);
	return bmp;
}

//-----------------------------------------------------
// Splashscreen
//-----------------------------------------------------

class clSplashScreen : public wxSplashScreen
{
	wxBitmap m_bmp;
	wxBitmap m_bgBmp;

public:
	clSplashScreen(const wxBitmap& bmp);
	virtual ~clSplashScreen();
	DECLARE_EVENT_TABLE()
	void OnPaint(wxPaintEvent &e);
	void OnEraseBackground(wxEraseEvent &e);
};

BEGIN_EVENT_TABLE(clSplashScreen, wxSplashScreen)
	EVT_PAINT(clSplashScreen::OnPaint)
	EVT_ERASE_BACKGROUND(clSplashScreen::OnEraseBackground)
END_EVENT_TABLE()

clSplashScreen::clSplashScreen(const wxBitmap& bmp)
#ifdef __WXMSW__
	: wxSplashScreen(bmp, wxSPLASH_CENTRE_ON_SCREEN|wxSPLASH_TIMEOUT, 5000, NULL, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxRESIZE_BORDER | wxBORDER_SIMPLE|wxFRAME_NO_TASKBAR| wxSTAY_ON_TOP)
#else
	: wxSplashScreen(bmp, wxSPLASH_CENTRE_ON_SCREEN|wxSPLASH_TIMEOUT, 5000, NULL, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE|wxFRAME_NO_TASKBAR| wxSTAY_ON_TOP)
#endif
	, m_bmp(bmp)
{
	SetSize(wxSize(m_bmp.GetWidth(), m_bmp.GetHeight()));
	Show(true);
    SetThemeEnabled(true);
    SetBackgroundStyle(wxBG_STYLE_CUSTOM);
}

clSplashScreen::~clSplashScreen()
{
}

void clSplashScreen::OnEraseBackground(wxEraseEvent& e)
{
}

void clSplashScreen::OnPaint(wxPaintEvent& e)
{
	wxPaintDC dc(this);
	dc.DrawBitmap(m_bmp, 0, 0, false);
}

///////////////////////////////////////////////////////////////////

#if wxVERSION_NUMBER < 2900
static const wxCmdLineEntryDesc cmdLineDesc[] = {
	{wxCMD_LINE_SWITCH, wxT("v"), wxT("version"), wxT("Print current version"), wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL },
	{wxCMD_LINE_SWITCH, wxT("h"), wxT("help"), wxT("Print usage"), wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL },
	{wxCMD_LINE_SWITCH, wxT("p"), wxT("no-plugins"), wxT("Start codelite without any plugins"), wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL },
	{wxCMD_LINE_OPTION, wxT("l"), wxT("line"), wxT("Open the file at a given line number"), wxCMD_LINE_VAL_NUMBER, wxCMD_LINE_PARAM_OPTIONAL },
	{wxCMD_LINE_OPTION, wxT("b"), wxT("basedir"),  wxT("Use this path as CodeLite installation path"), wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL },
	{wxCMD_LINE_PARAM,  NULL, NULL, wxT("Input file"), wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_MULTIPLE|wxCMD_LINE_PARAM_OPTIONAL },
	{wxCMD_LINE_NONE }
};
#else
static const wxCmdLineEntryDesc cmdLineDesc[] = {
	{wxCMD_LINE_SWITCH, "v", "version", "Print current version", wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL },
	{wxCMD_LINE_SWITCH, "h", "help", "Print usage", wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL },
	{wxCMD_LINE_SWITCH, "p", "no-plugins", "Start codelite without any plugins", wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL },
	{wxCMD_LINE_OPTION, "l", "line", "Open the file at a given line number", wxCMD_LINE_VAL_NUMBER, wxCMD_LINE_PARAM_OPTIONAL },
	{wxCMD_LINE_OPTION, "b", "basedir",  "The base directory of CodeLite", wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_OPTIONAL },
	{wxCMD_LINE_PARAM,  NULL, NULL, "Input file", wxCMD_LINE_VAL_STRING, wxCMD_LINE_PARAM_MULTIPLE|wxCMD_LINE_PARAM_OPTIONAL },
	{wxCMD_LINE_NONE }
};
#endif

static void massCopy(const wxString &sourceDir, const wxString &spec, const wxString &destDir)
{
	wxArrayString files;
	wxDir::GetAllFiles(sourceDir, &files, spec, wxDIR_FILES);
	for ( size_t i=0; i<files.GetCount(); i++ ) {
		wxFileName fn(files.Item(i));
		wxCopyFile(files.Item(i), destDir + wxT("/") + fn.GetFullName());
	}
}

#ifdef __WXGTK__
//-------------------------------------------
// Signal Handlers for GTK
//-------------------------------------------
static void WaitForDebugger(int signo)
{
	wxString msg;
    wxString where;

	msg << wxT("codelite crashed: you may attach to it using gdb\n")
	    << wxT("or let it crash silently..\n")
	    << wxT("Attach debugger?\n");

	int rc = wxMessageBox(msg, wxT("CodeLite Crash Handler"), wxYES_NO|wxCENTER|wxICON_ERROR);
	if(rc == wxYES) {

		// Launch a shell command with the following command:
		// gdb -p <PID>

		char command[256];
		memset (command, 0, sizeof(command));

        if (ExeLocator::Locate(wxT("gnome-terminal"), where)) {
            sprintf(command, "gnome-terminal -t 'gdb' -e 'gdb -p %d'", getpid());
        } else if (ExeLocator::Locate(wxT("konsole"), where)) {
            sprintf(command, "konsole -T 'gdb' -e 'gdb -p %d'", getpid());
        } else if (ExeLocator::Locate(wxT("terminal"), where)) {
            sprintf(command, "terminal -T 'gdb' -e 'gdb -p %d'", getpid());
        } else if (ExeLocator::Locate(wxT("lxterminal"), where)) {
            sprintf(command, "lxterminal -T 'gdb' -e 'gdb -p %d'", getpid());
        } else {
            sprintf(command, "xterm -T 'gdb' -e 'gdb -p %d'", getpid());
        }

		if(system (command) == 0) {
			signal(signo, SIG_DFL);
			raise(signo);
		} else {
			// Go down without launching the debugger, ask the user to do it manually
			wxMessageBox(wxString::Format(wxT("Failed to launch the debugger\nYou may still attach to codelite manually by typing this command in a terminal:\ngdb -p %d"), getpid()), wxT("CodeLite Crash Handler"), wxOK|wxCENTER|wxICON_ERROR);
			pause();
		}
	}

	signal(signo, SIG_DFL);
	raise(signo);
}
#endif

#if defined(__WXGTK__) || defined(__WXMAC__)
static void ChildTerminatedSingalHandler(int signo)
{
	int status;
	while( true ) {
		pid_t pid = waitpid(-1, &status, WNOHANG);
		if(pid > 0) {
			// waitpid succeeded
			IProcess::SetProcessExitCode(pid, WEXITSTATUS(status));

		} else {
			break;

		}
	}

	// reinstall the handler
	signal(SIGCHLD, ChildTerminatedSingalHandler);
}

#endif

IMPLEMENT_APP(CodeLiteApp)

BEGIN_EVENT_TABLE(CodeLiteApp, wxApp)
EVT_ACTIVATE_APP(CodeLiteApp::OnAppAcitvated)
END_EVENT_TABLE()

extern void InitXmlResource();
CodeLiteApp::CodeLiteApp(void)
	: m_pMainFrame(NULL)
	, m_singleInstance(NULL)
	, m_loadPlugins(true)
#ifdef __WXMSW__
	, m_handler(NULL)
#endif
{
}

CodeLiteApp::~CodeLiteApp(void)
{
	wxImage::CleanUpHandlers();
#ifdef __WXMSW__
	if (m_handler) {
		FreeLibrary(m_handler);
		m_handler = NULL;
	}
#endif
	if ( m_singleInstance ) {
		delete m_singleInstance;
	}
	wxAppBase::ExitMainLoop();
}

bool CodeLiteApp::OnInit()
{
#if defined(__WXGTK__) || defined(__WXMAC__)
	// block signal pipe
	sigset_t mask_set;
	sigemptyset( &mask_set );
	sigaddset(&mask_set, SIGPIPE);
	sigprocmask(SIG_SETMASK, &mask_set, NULL);

	// Handle sigchld
	signal(SIGCHLD, ChildTerminatedSingalHandler);

#ifdef __WXGTK__
	// Insall signal handlers
	signal(SIGSEGV, WaitForDebugger);
	signal(SIGABRT, WaitForDebugger);
#endif

#endif

	wxSocketBase::Initialize();

#if wxUSE_ON_FATAL_EXCEPTION
	//trun on fatal exceptions handler
	wxHandleFatalExceptions(true);
#endif

#ifdef __WXMSW__
	// as described in http://jrfonseca.dyndns.org/projects/gnu-win32/software/drmingw/
	// load the exception handler dll so we will get Dr MinGW at runtime
	//m_handler = LoadLibrary(wxT("exchndl.dll"));

	// Enable this process debugging priviliges
	//EnableDebugPriv();
#endif


	// Init resources and add the PNG handler
	wxSystemOptions::SetOption(_T("msw.remap"), 0);
	wxXmlResource::Get()->InitAllHandlers();
	wxImage::AddHandler( new wxPNGHandler );
	wxImage::AddHandler( new wxCURHandler );
	wxImage::AddHandler( new wxICOHandler );
	wxImage::AddHandler( new wxXPMHandler );
	wxImage::AddHandler( new wxGIFHandler );
	InitXmlResource();

	wxLog::EnableLogging(false);
	wxString homeDir(wxEmptyString);

	//parse command line
	wxCmdLineParser parser;
	parser.SetDesc(cmdLineDesc);
	parser.SetCmdLine(wxAppBase::argc, wxAppBase::argv);
	if (parser.Parse() != 0) {
		return false;
	}

	if (parser.Found(wxT("h"))) {
		// print usage
		parser.Usage();
		return false;
	}

	if (parser.Found(wxT("p"))) {
		// Load codelite without plugins
		SetLoadPlugins(false);
	}

	wxString newBaseDir(wxEmptyString);
	if (parser.Found(wxT("b"), &newBaseDir)) {
		homeDir = newBaseDir;
	}

#if defined (__WXGTK__)
	if (homeDir.IsEmpty()) {
		SetAppName(wxT("codelite"));
		homeDir = wxStandardPaths::Get().GetUserDataDir(); // ~/Library/Application Support/codelite or ~/.codelite

		//Create the directory structure
		wxLogNull noLog;
		wxMkdir(homeDir);
		wxMkdir(homeDir + wxT("/lexers/"));
		wxMkdir(homeDir + wxT("/rc/"));
		wxMkdir(homeDir + wxT("/images/"));
		wxMkdir(homeDir + wxT("/templates/"));
		wxMkdir(homeDir + wxT("/config/"));
		wxMkdir(homeDir + wxT("/tabgroups/"));

		//copy the settings from the global location if needed
		wxString installPath( INSTALL_DIR, wxConvUTF8 );
		if ( ! CopySettings(homeDir, installPath ) ) return false;
		ManagerST::Get()->SetInstallDir( installPath );

	} else {
		wxFileName fn(homeDir);
		fn.MakeAbsolute();
		ManagerST::Get()->SetInstallDir( fn.GetFullPath() );
	}

#elif defined (__WXMAC__)
	SetAppName(wxT("codelite"));
	homeDir = wxStandardPaths::Get().GetUserDataDir();

	{
		wxLogNull noLog;

		//Create the directory structure
		wxMkdir(homeDir);
		wxMkdir(homeDir + wxT("/lexers/"));
		wxMkdir(homeDir + wxT("/rc/"));
		wxMkdir(homeDir + wxT("/images/"));
		wxMkdir(homeDir + wxT("/templates/"));
		wxMkdir(homeDir + wxT("/config/"));
		wxMkdir(homeDir + wxT("/tabgroups/"));
	}

	wxString installPath( MacGetBasePath() );
	ManagerST::Get()->SetInstallDir( installPath );
	//copy the settings from the global location if needed
	CopySettings(homeDir, installPath);

#else //__WXMSW__
	if (homeDir.IsEmpty()) { //did we got a basedir from user?
		homeDir = ::wxGetCwd();
	}
	wxFileName fnHomdDir(homeDir + wxT("/"));

	// try to locate the menu/rc.xrc file
	wxFileName fn(homeDir + wxT("/rc"), wxT("menu.xrc"));
	if (!fn.FileExists()) {
		// we got wrong home directory
		wxFileName appFn( wxAppBase::argv[0] );
		homeDir = appFn.GetPath();
	}

	if (fnHomdDir.IsRelative()) {
		fnHomdDir.MakeAbsolute();
		homeDir = fnHomdDir.GetPath();
	}

	ManagerST::Get()->SetInstallDir( homeDir );
#endif

	// Update codelite revision and Version
	EditorConfigST::Get()->Init(SvnRevision, wxT("2.0.2") );

	ManagerST::Get()->SetOriginalCwd(wxGetCwd());
	::wxSetWorkingDirectory(homeDir);
	// Load all of the XRC files that will be used. You can put everything
	// into one giant XRC file if you wanted, but then they become more
	// diffcult to manage, and harder to reuse in later projects.
	// The menubar
	if (!wxXmlResource::Get()->Load( DoFindMenuFile(ManagerST::Get()->GetInstallDir(), wxT("2.0")) ) )
		return false;

	// keep the startup directory
	ManagerST::Get()->SetStarupDirectory(::wxGetCwd());

	// set the performance output file name
	PERF_OUTPUT(wxString::Format(wxT("%s/codelite.perf"), wxGetCwd().c_str()).mb_str(wxConvUTF8));

	// Initialize the configuration file locater
	ConfFileLocator::Instance()->Initialize(ManagerST::Get()->GetInstallDir(), ManagerST::Get()->GetStarupDirectory());

	Manager *mgr = ManagerST::Get();

	// set the CTAGS_REPLACEMENT environment variable
	wxSetEnv(wxT("CTAGS_REPLACEMENTS"), ManagerST::Get()->GetStarupDirectory() + wxT("/ctags.replacements"));

	long style = wxSIMPLE_BORDER;
#if defined (__WXMSW__) || defined (__WXGTK__)
	style |= wxFRAME_NO_TASKBAR;

#else // Mac
	wxUnusedVar(style);

#endif

	//read the last frame size from the configuration file
	// Initialise editor configuration files
#ifdef __WXMSW__
	{
		wxLogNull noLog;
		wxMkdir(wxStandardPaths::Get().GetUserDataDir());
	}
#endif

	EditorConfigST::Get()->SetInstallDir( mgr->GetInstallDir() );
	EditorConfig *cfg = EditorConfigST::Get();
	if ( !cfg->Load() ) {
		CL_ERROR(wxT("Failed to load configuration file: %s/config/codelite.xml"), wxGetCwd().c_str());
		return false;
	}

	// Set the log file verbosity
	long log_verbosity(FileLogger::Error);
	EditorConfigST::Get()->GetLongValue(wxT("LogVerbosity"), log_verbosity);
	FileLogger::Get()->SetVerbosity(log_verbosity);
	CL_SYSTEM(wxT("Starting codelite..."));

	// check for single instance
	if ( !CheckSingularity(parser, ManagerST::Get()->GetOriginalCwd()) ) {
		return false;
	}

	//---------------------------------------------------------
	// Set environment variable for CodeLiteDir (make it first
	// on the list so it can be used by other variables)
	//---------------------------------------------------------
	EvnVarList vars;
	EnvironmentConfig::Instance()->Load();
	EnvironmentConfig::Instance()->ReadObject(wxT("Variables"), &vars);

	vars.InsertVariable(wxT("Default"), wxT("CodeLiteDir"), ManagerST::Get()->GetInstallDir());
	EnvironmentConfig::Instance()->WriteObject(wxT("Variables"), &vars);

	//---------------------------------------------------------

#ifdef __WXMSW__

	// Read registry values
	MSWReadRegistry();

#endif

	GeneralInfo inf;
	cfg->ReadObject(wxT("GeneralInfo"), &inf);

	// Set up the locale if appropriate
	if (EditorConfigST::Get()->GetOptions()->GetUseLocale()) {
		int preferredLocale = wxLANGUAGE_ENGLISH;
		// The locale had to be saved as the canonical locale name, as the wxLanguage enum wasn't consistent between wx versions
		wxString preferredLocalename = EditorConfigST::Get()->GetOptions()->GetPreferredLocale();
		if (!preferredLocalename.IsEmpty()) {
			const wxLanguageInfo* info = wxLocale::FindLanguageInfo(preferredLocalename);
			if (info) {
				preferredLocale = info->Language;
				if (preferredLocale == wxLANGUAGE_UNKNOWN) {
					preferredLocale = wxLANGUAGE_ENGLISH;
				}
			}
		}

#if defined (__WXGTK__)
		// Cater for a --prefix= build. This gets added automatically to the search path for catalogues.
		// So hack in the standard ones too, otherwise wxstd.mo will be missed
		wxLocale::AddCatalogLookupPathPrefix(wxT("/usr/share/locale"));
		wxLocale::AddCatalogLookupPathPrefix(wxT("/usr/local/share/locale"));

#elif defined(__WXMSW__)
		wxLocale::AddCatalogLookupPathPrefix(ManagerST::Get()->GetInstallDir() + wxT("\\locale"));
#endif

		// This has to be done before the catalogues are added, as otherwise the wrong one (or none) will be found
		m_locale.Init(preferredLocale);

		bool codelitemo_found = m_locale.AddCatalog(wxT("codelite"));
		if (!codelitemo_found) {
			m_locale.AddCatalog(wxT("CodeLite")); // Hedge bets re our spelling
		}

		if (!codelitemo_found) {
			// I wanted to 'un-init' the locale if no translations were found
			// as otherwise, in a RTL locale, menus, dialogs etc will be displayed RTL, in English...
			// However I couldn't find a way to do this
		}
	}


	// show splashscreen here
	bool showSplash;

#if wxDEBUG_LEVEL == 0
	showSplash = inf.GetFlags() & CL_SHOW_SPLASH ? true : false;
#else
	showSplash = false;
#endif

	// Don't show the splash screen when opening codelite to view
	// a file, this is done to reduce the load time
	if(parser.GetParamCount() > 0)
		showSplash = false;

	m_splash = NULL;
	if (showSplash) {
		wxBitmap bitmap;
		wxString splashName(mgr->GetStarupDirectory() + wxT("/images/splashscreen.png"));
		if (bitmap.LoadFile(splashName, wxBITMAP_TYPE_PNG)) {
			wxString mainTitle = CODELITE_VERSION_STR;
			wxBitmap splash = clDrawSplashBitmap(bitmap, mainTitle);
			m_splash = new clSplashScreen(splash);
		}
	}

	// Create the main application window
	clMainFrame::Initialize( parser.GetParamCount() == 0 );
	m_pMainFrame = clMainFrame::Get();
	m_pMainFrame->Show(TRUE);
	SetTopWindow(m_pMainFrame);

	long lineNumber(0);
	parser.Found(wxT("l"), &lineNumber);
	if (lineNumber > 0) {
		lineNumber--;
	} else {
		lineNumber = 0;
	}

	for (size_t i=0; i< parser.GetParamCount(); i++) {
		wxString argument = parser.GetParam(i);

		//convert to full path and open it
		wxFileName fn(argument);
		fn.MakeAbsolute(ManagerST::Get()->GetOriginalCwd());

		if (fn.GetExt() == wxT("workspace")) {
			ManagerST::Get()->OpenWorkspace(fn.GetFullPath());
		} else {
			clMainFrame::Get()->GetMainBook()->OpenFile(fn.GetFullPath(), wxEmptyString, lineNumber);
		}
	}

	wxLogMessage(wxString::Format(wxT("Install path: %s"), ManagerST::Get()->GetInstallDir().c_str()));
	wxLogMessage(wxString::Format(wxT("Startup Path: %s"), ManagerST::Get()->GetStarupDirectory().c_str()));

#ifdef __WXGTK__
	// Needed on GTK
	if (clMainFrame::Get()->GetMainBook()->GetActiveEditor() == NULL) {
		clMainFrame::Get()->GetOutputPane()->GetBuildTab()->SetFocus();
	}
#endif

	// Especially with the OutputView open, CodeLite was consuming 50% of a cpu, mostly in updateui
	// The next line limits the frequency of UpdateUI events to every 100ms
	wxUpdateUIEvent::SetUpdateInterval(100);

	return TRUE;
}

int CodeLiteApp::OnExit()
{
	CL_SYSTEM(wxT("Bye"));
	EditorConfigST::Free();
	ConfFileLocator::Release();
	return 0;
}

bool CodeLiteApp::CopySettings(const wxString &destDir, wxString& installPath)
{
	wxLogNull noLog;

	///////////////////////////////////////////////////////////////////////////////////////////
	// copy new settings from the global installation location which is currently located at
	// /usr/local/share/codelite/ (Linux) or at codelite.app/Contents/SharedSupport
	///////////////////////////////////////////////////////////////////////////////////////////
	CopyDir(installPath + wxT("/templates/"), destDir + wxT("/templates/"));
	massCopy  (installPath + wxT("/images/"), wxT("*.png"), destDir + wxT("/images/"));
	//wxCopyFile(installPath + wxT("/rc/menu.xrc"), destDir + wxT("/rc/menu.xrc"));
	wxCopyFile(installPath + wxT("/index.html"), destDir + wxT("/index.html"));
	wxCopyFile(installPath + wxT("/svnreport.html"), destDir + wxT("/svnreport.html"));
	wxCopyFile(installPath + wxT("/astyle.sample"), destDir + wxT("/astyle.sample"));
	return true;
}

void CodeLiteApp::OnFatalException()
{
#if wxUSE_STACKWALKER
	Manager *mgr = ManagerST::Get();
	wxString startdir = mgr->GetStarupDirectory();
	startdir << wxT("/crash.log");

	wxFileOutputStream outfile(startdir);
	wxTextOutputStream out(outfile);
	out.WriteString(wxDateTime::Now().FormatTime() + wxT("\n"));
	StackWalker walker(&out);
	walker.Walk();
	wxAppBase::ExitMainLoop();
#endif
}

bool CodeLiteApp::CheckSingularity(const wxCmdLineParser &parser, const wxString &curdir)
{
	// check for single instance
	long singleInstance(1);
	EditorConfigST::Get()->GetLongValue(wxT("SingleInstance"), singleInstance);
	if ( singleInstance ) {
		const wxString name = wxString::Format(wxT("CodeLite-%s"), wxGetUserId().c_str());

		m_singleInstance = new wxSingleInstanceChecker(name);
		if (m_singleInstance->IsAnotherRunning()) {
			// prepare commands file for the running instance
			wxString files;
			for (size_t i=0; i< parser.GetParamCount(); i++) {
				wxString argument = parser.GetParam(i);

				//convert to full path and open it
				wxFileName fn(argument);
				fn.MakeAbsolute(curdir);
				files << fn.GetFullPath() << wxT("\n");
			}

			if (files.IsEmpty() == false) {
				Mkdir(ManagerST::Get()->GetStarupDirectory() + wxT("/ipc"));

				wxString file_name, tmp_file;
				tmp_file 	<< ManagerST::Get()->GetStarupDirectory()
				            << wxT("/ipc/command.msg.tmp");

				file_name 	<< ManagerST::Get()->GetStarupDirectory()
				            << wxT("/ipc/command.msg");

				// write the content to a temporary file, once completed,
				// rename the file to the actual file name
				WriteFileUTF8(tmp_file, files);
				wxRenameFile(tmp_file, file_name);
			}
			return false;
		}
	}
	return true;
}

void CodeLiteApp::MacOpenFile(const wxString& fileName)
{
	switch (FileExtManager::GetType(fileName)) {
	case FileExtManager::TypeWorkspace:
		ManagerST::Get()->OpenWorkspace(fileName);
		break;
	default:
		clMainFrame::Get()->GetMainBook()->OpenFile(fileName);
		break;
	}
}

void CodeLiteApp::MSWReadRegistry()
{
#ifdef __WXMSW__

	EvnVarList vars;
	EnvironmentConfig::Instance()->Load();
	EnvironmentConfig::Instance()->ReadObject(wxT("Variables"), &vars);

	/////////////////////////////////////////////////////////////////
	// New way of registry:
	// Read the registry INI file
	/////////////////////////////////////////////////////////////////

	// Update PATH environment variable with the install directory and
	// MinGW default installation (if exists)
	wxString pathEnv;
	wxGetEnv(wxT("PATH"), &pathEnv);
	pathEnv << wxT(";") << ManagerST::Get()->GetInstallDir() << wxT(";");

	// Load the registry file
	wxString iniFile;
	iniFile << ManagerST::Get()->GetInstallDir() << wxFileName::GetPathSeparator() << wxT("registry.ini");

	if (wxFileName::FileExists(iniFile)) {
		clRegistry::SetFilename(iniFile);
		clRegistry registry;

		m_parserPaths.Clear();
		wxString strWx, strMingw, strUnitTestPP;
		registry.Read(wxT("wx"),         strWx);
		registry.Read(wxT("mingw"),      strMingw);
		registry.Read(wxT("unittestpp"), strUnitTestPP);

		// Supprot for wxWidgets
		if (strWx.IsEmpty() == false) {
			// we have WX installed on this machine, set the path of WXWIN & WXCFG to point to it
			EnvMap envs = vars.GetVariables(wxT("Default"), false, wxT(""));

			if (!envs.Contains(wxT("WXWIN"))) {
				vars.AddVariable(wxT("Default"), wxT("WXWIN"), strWx);
				vars.AddVariable(wxT("Default"), wxT("PATH"),  wxT("$(WXWIN)\\lib\\gcc_dll;$(PATH)"));
			}

			if (!envs.Contains(wxT("WXCFG")))
				vars.AddVariable(wxT("Default"), wxT("WXCFG"), wxT("gcc_dll\\mswu"));

			EnvironmentConfig::Instance()->WriteObject(wxT("Variables"), &vars);
			wxSetEnv(wxT("WX_INCL_HOME"), strWx + wxT("\\include"));
		}

		// Support for UnitTest++
		if (strUnitTestPP.IsEmpty() == false) {
			// we have UnitTest++ installed on this machine
			EnvMap envs = vars.GetVariables(wxT("Default"), false, wxT(""));

			if (!envs.Contains(wxT("UNIT_TEST_PP_SRC_DIR")) ) {
				vars.AddVariable(wxT("Default"), wxT("UNIT_TEST_PP_SRC_DIR"), strUnitTestPP);
			}

			EnvironmentConfig::Instance()->WriteObject(wxT("Variables"), &vars);
		}

		// Support for MinGW
		if (strMingw.IsEmpty() == false) {
			// Add the installation include paths
			pathEnv << wxT(";") << strMingw << wxT("\\bin");
			wxSetEnv(wxT("MINGW_INCL_HOME"), strMingw);
		}

		wxSetEnv(wxT("PATH"), pathEnv);

	} else {

		// Use the old way: read values from registry. This code is kept so people using
		// SVN build of codelite can continue working with it until an official build is released
		// Update PATH environment variable with the install directory and
		// MinGW default installation (if exists)
		wxString pathEnv;
		if (wxGetEnv(wxT("PATH"), &pathEnv) == false) {
			wxLogMessage(_("WARNING: Failed to load environment variable PATH!"));
		} else {
			pathEnv << wxT(";") << ManagerST::Get()->GetInstallDir() << wxT(";");

			// read the installation path of MinGW & WX
			wxRegKey rk(wxT("HKEY_CURRENT_USER\\Software\\CodeLite"));
			if (rk.Exists()) {
				m_parserPaths.Clear();
				wxString strWx, strMingw, strUnitTestPP;
				if (rk.HasValue(wxT("wx"))) {
					rk.QueryValue(wxT("wx"), strWx);
				}

				if (rk.HasValue(wxT("mingw"))) {
					rk.QueryValue(wxT("mingw"), strMingw);
				}

				if (rk.HasValue(wxT("unittestpp"))) {
					rk.QueryValue(wxT("unittestpp"), strUnitTestPP);
				}



				if (strWx.IsEmpty() == false) {
					// we have WX installed on this machine, set the path of WXWIN & WXCFG to point to it
					EnvMap envs = vars.GetVariables(wxT("Default"), false, wxT(""));

					if (envs.Contains(wxT("WXWIN")) == false) {
						vars.AddVariable(wxT("Default"), wxT("WXWIN"), strWx);
						vars.AddVariable(wxT("Default"), wxT("PATH"),  wxT("$(WXWIN)\\lib\\gcc_dll;$(PATH)"));
					}

					if (envs.Contains(wxT("WXCFG")) == false)
						vars.AddVariable(wxT("Default"), wxT("WXCFG"), wxT("gcc_dll\\mswu"));

					EnvironmentConfig::Instance()->WriteObject(wxT("Variables"), &vars);
					wxSetEnv(wxT("WX_INCL_HOME"), strWx + wxT("\\include"));
				}

				if (strMingw.IsEmpty() == false) {
					// Add the installation include paths
					pathEnv << wxT(";") << strMingw << wxT("\\bin");
					wxSetEnv(wxT("MINGW_INCL_HOME"), strMingw);
				}

				// Support for UnitTest++
				if (strUnitTestPP.IsEmpty() == false) {
					// we have UnitTest++ installed on this machine
					EnvMap envs = vars.GetVariables(wxT("Default"), false, wxT(""));

					if (!envs.Contains(wxT("UNIT_TEST_PP_SRC_DIR")) ) {
						vars.AddVariable(wxT("Default"), wxT("UNIT_TEST_PP_SRC_DIR"), strUnitTestPP);
					}

					EnvironmentConfig::Instance()->WriteObject(wxT("Variables"), &vars);
				}
			}

			if (wxSetEnv(wxT("PATH"), pathEnv) == false) {
				wxLogMessage(_("WARNING: Failed to update environment variable PATH"));
			}
		}
	}
#endif
}

wxString CodeLiteApp::DoFindMenuFile(const wxString& installDirectory, const wxString &requiredVersion)
{
	wxString defaultMenuFile = installDirectory + wxFileName::GetPathSeparator() + wxT("rc") + wxFileName::GetPathSeparator() + wxT("menu.xrc");
	wxFileName menuFile(wxStandardPaths::Get().GetUserDataDir() + wxFileName::GetPathSeparator() + wxT("rc") + wxFileName::GetPathSeparator() + wxT("menu.xrc"));
	if(menuFile.FileExists()) {
		// if we find the user's file menu, check that it has the required version
		{
			wxLogNull noLog;
			wxXmlDocument doc;
			if(doc.Load(menuFile.GetFullPath())) {
				wxString version = doc.GetRoot()->GetPropVal(wxT("version"), wxT("1.0"));
				if(version != requiredVersion) {
					return defaultMenuFile;
				}
			}
		}
		return menuFile.GetFullPath();
	}
	return defaultMenuFile;
}

void CodeLiteApp::OnAppAcitvated(wxActivateEvent& e)
{
    if(e.GetActive()) {

        if(clMainFrame::Get()) {
            SetTopWindow(clMainFrame::Get());
        }

        if(ManagerST::Get()->IsWorkspaceOpen() && !ManagerST::Get()->IsWorkspaceClosing()) {
            // Retag the workspace the light way
            ManagerST::Get()->RetagWorkspace(TagsManager::Retag_Quick_No_Scan);
        }
    }
}
