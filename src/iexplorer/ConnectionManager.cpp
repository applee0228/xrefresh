#include "StdAfx.h"
#include "ConnectionManager.h"
#include "Logger.h"
#include "XRefreshBHO.h"
#include "BrowserManager.h"

#ifndef _UNICODE
#error "_UNICODE required"
#endif

// helpers
CString UnpackValue(Json::Value& v)
{
	// we expect we are in UNICODE mode
	const char* c = v.asCString();
	if (!c) return CString();
	int s = strlen(c);
	int l = MultiByteToWideChar(CP_UTF8, 0, c, s, 0, 0);
	wchar_t* buf = new wchar_t[l+1];
	CAPtrGuard guard(buf);
	MultiByteToWideChar(CP_UTF8, 0, c, s, buf, l);
	buf[l] = 0;
	return CString(buf);
}

Json::Value PackValue(CString v)
{
	int vl = v.GetLength();
	int l = WideCharToMultiByte(CP_UTF8, 0, v, vl, 0, 0, NULL, NULL);
	if (!l) return Json::Value();
	char* buf = new char[l+1];
	CAPtrGuard guard(buf);
	WideCharToMultiByte(CP_UTF8, 0, v, vl, buf, l, NULL, NULL);
	buf[l] = 0;
	return Json::Value(buf);
}

CConnection::CConnection(CConnectionManager* parent):
	m_Parent(parent),
	m_BufferPos(0)
{
}

CConnection::~CConnection()
{

}

void 
CConnection::OnDataReceived(const LPBYTE lpBuffer, DWORD dwCount)
{
	if (m_BufferPos+dwCount+1>=CONNECTION_BUFFER_SIZE)
	{
		m_Parent->Log(_T("Connection buffer is too small. Message data has been dropped."), ICON_ERROR);
		m_BufferPos = 0;
		return;
	}
	memcpy(m_Buffer + m_BufferPos, lpBuffer, dwCount);
	m_BufferPos += dwCount;

	Json::Value msg;
	Json::Reader reader;
	bool res = reader.parse(m_Buffer, m_Buffer+m_BufferPos, msg, false);
	if (!res)
	{
		// we have only partial message ? store data and wait for next chunk ...
		return;
	}
	m_BufferPos = 0;
	m_Parent->ProcessMessage(msg);
}

void 
CConnection::OnEvent(UINT uEvent)
{
	BrowserManagerLock browserManager;
	CBrowserMessageWindow* window = browserManager->FindBrowserMessageWindow(m_Parent->m_Parent->GetBrowserId());
	if (!window) return;

	switch (uEvent)
	{
		case EVT_CONSUCCESS:
			TRACE_I( _T("Connection Established\r\n") );
			break;
		case EVT_CONFAILURE:
			TRACE_I( _T("Connection Failed\r\n") );
			window->PostMessage(BMM_REQUEST_DISCONNECTED_NOTIFY, 0, 0);
			CloseComm();
			m_Parent->m_Connected = false;
			break;
		case EVT_CONDROP:
			TRACE_I( _T("Connection Abandonned\r\n") );
			window->PostMessage(BMM_REQUEST_DISCONNECTED_NOTIFY, 0, 0);
			CloseComm();
			m_Parent->m_Connected = false;
			break;
		case EVT_ZEROLENGTH:
			TRACE_I( _T("Zero Length Message\r\n") );
			break;
		default:
			TRACE_W(_T("Unknown Socket event\n"));
			break;
	}
}

/////////////////////////////////////////////////////////////////////////////////////

CReconnectListener::CReconnectListener(CConnectionManager* parent):
	m_Parent(parent)
{
}

CReconnectListener::~CReconnectListener()
{

}

void 
CReconnectListener::OnDataReceived(const LPBYTE lpBuffer, DWORD dwCount)
{
}

void 
CReconnectListener::OnEvent(UINT uEvent)
{
	switch (uEvent)
	{
		case EVT_CONSUCCESS:
			TRACE_I( _T("Reconnection request") );
			m_Parent->Reconnect();
			break;
		case EVT_CONFAILURE:
			CloseComm();
			m_Parent->RequestStartingReconnectListener();
			break;
		case EVT_CONDROP:
			CloseComm();
			m_Parent->RequestStartingReconnectListener();
			break;
		case EVT_ZEROLENGTH:
			break;
		default:
			TRACE_W(_T("Unknown Socket event\n"));
			break;
	}
}

///////////////////////////////////////////////////////////////////////////////

CConnectionManager::CConnectionManager(CXRefreshBHO* parent):
m_Connection(this),
m_ReconnectListener(this),
m_Connected(false),
m_Parent(parent)
{
	StartReconnectListener();
}

CConnectionManager::~CConnectionManager()
{
	StopReconnectListener();
}

void
CConnectionManager::Connect()
{
	if (m_Connection.IsOpen()) 
	{
		m_Parent->Log(_T("Already connected."), ICON_INFO);
		return;
	}
	TRACE_I(_T("Trying to connect ..."));
	DWORD size = 1024;
	TCHAR host[1024] = _T("localhost");
	if (GetStringValueFromRegistry(REGISTRY_ROOT_KEY, REGISTRY_SETTINGS_KEY, REGISTRY_SETTINGS_HOST, host, &size)!=0)
	{
		m_Parent->Log(_T("Unable to read host record from registry."), ICON_WARNING);
	}

	DWORD port = 41258;
	if (GetDWORDValueFromRegistry(REGISTRY_ROOT_KEY, REGISTRY_SETTINGS_KEY, REGISTRY_SETTINGS_PORT, &port)!=0)
	{
		m_Parent->Log(_T("Unable to read port from registry."), ICON_WARNING);
	}

	CString sport;
	sport.Format(_T("%d"), port);
	if (!m_Connection.ConnectTo(host, sport, AF_INET, SOCK_STREAM))
	{
		CString msg;
		msg.Format(_T("Unable to see XRefresh Monitor."));
		m_Parent->Log(msg, ICON_WARNING);
		msg.Format(_T("Please check if you have running XRefresh Monitor. On Windows, it is program running in system tray. Look for Programs -> XRefresh -> XRefresh.exe"));
		m_Parent->Log(msg, ICON_BULB);
		msg.Format(_T("You may also want to check your firewall settings. XRefresh IE Addon expects Monitor to talk from %s on port %d"), host, port);
		m_Parent->Log(msg, ICON_BULB);
		TRACE_I(_T("server not available"));
		return;
	}
	m_Connection.WatchComm();
	SendHello();
}

void
CConnectionManager::Disconnect()
{
	TRACE_I(_T("Trying to disconnect ..."));
	if (!m_Connection.IsOpen()) return;
	SendBye();
	m_Connection.StopComm();
	UpdateIcon();
}

bool
CConnectionManager::IsConnected()
{
	return m_Connected;
}

void
CConnectionManager::Send(Json::Value& msg)
{
	if (!m_Connection.IsOpen()) return;

	Json::FastWriter writer;
	string data = writer.write(msg);
	if (!m_Connection.WriteComm((LPBYTE)data.c_str(), data.size(), 1000))
	{
		TRACE_E(_T("unable to send message"));
	}
}

void
CConnectionManager::SendHello()
{
	Json::Value msg;
	msg["command"] = "Hello";
	msg["type"] = "Internet Explorer";
	msg["agent"] = "Agent?";
	Send(msg);
}

void
CConnectionManager::SendBye()
{
	Json::Value msg;
	msg["command"] = "Bye";
	Send(msg);
}

void
CConnectionManager::SendSetPage(CString title, CString url)
{
	Json::Value msg;
	msg["command"] = "SetPage";
	msg["page"] = PackValue(title);
	msg["url"] = PackValue(url);
	Send(msg);
}

CString 
CConnectionManager::FileSentence(int n, CString s)
{
	CString r;
	if (n==0) return r;
	if (n==1) 
	{
		r.Format(_T("one item %s"), s);
		return r;
	}
	r.Format(_T("%d items %s"), n, s);
	return r;
}

CString 
CConnectionManager::GetStory(Json::Value& msg)
{
	int changed = 0;
	int created = 0;
	int deleted = 0;
	int renamed = 0;
	Json::Value files = msg["files"];
	for (unsigned int i=0; i<files.size(); i++)
	{
		Json::Value file = files[i];
		CString action = UnpackValue(file["action"]);
		if (action==_T("changed")) changed++;
		if (action==_T("created")) created++;
		if (action==_T("deleted")) deleted++;
		if (action==_T("renamed")) renamed++;
	}
	
	CString fs;
	CString s[4];
	int i = 0;
	fs = FileSentence(created, _T("created"));
	if (fs.GetLength()) s[i++] = fs;
	fs = FileSentence(deleted, _T("deleted"));
	if (fs.GetLength()) s[i++] = fs;
	fs = FileSentence(changed, _T("changed"));
	if (fs.GetLength()) s[i++] = fs;
	fs = FileSentence(renamed, _T("renamed"));
	if (fs.GetLength()) s[i++] = fs;
	
	CString story;
	if (i==0) return _T("?");
	if (i==1) story.Format(_T("%s"), s[0]);
	if (i==2) story.Format(_T("%s and %s"), s[0], s[1]);
	if (i==3) story.Format(_T("%s, %s and %s"), s[0], s[1], s[2]);
	if (i==4) story.Format(_T("%s, %s, %s and %s"), s[0], s[1], s[2], s[3], s[4]);
	return story;
}

void
CConnectionManager::ProcessMessage(Json::Value& msg)
{
	CString command = UnpackValue(msg["command"]);
	if (command==_T("AboutMe"))
	{
		m_Connected = true;
		m_Agent = UnpackValue(msg["agent"]);
		m_Version = UnpackValue(msg["version"]);
		CString log;
		log.Format(_T("Connected to XRefresh Monitor: %s %s"), m_Agent, m_Version);
		m_Parent->Log(log, ICON_CONNECTED);
		RequestSendInfoAboutPage();
		UpdateIcon();
	}
	if (command==_T("DoRefresh"))
	{
		CString name = UnpackValue(msg["name"]);
		CString story = GetStory(msg);
		CString log;
		log.Format(_T("Refresh request from %s: %s"), name, story);
		m_Parent->Log(log, ICON_REFRESH);
		PerformRefresh();
	}
}

void
CConnectionManager::UpdateIcon()
{
	BrowserManagerLock browserManager;
	CBrowserMessageWindow* window = browserManager->FindBrowserMessageWindow(m_Parent->GetBrowserId());
	if (!window) return;

	// refresh must be called by BHO thread
	window->PostMessage(BMM_REQUEST_UPDATE_ICON, 0, 0);
}

void 
CConnectionManager::RequestSendInfoAboutPage()
{
	BrowserManagerLock browserManager;
	CBrowserMessageWindow* window = browserManager->FindBrowserMessageWindow(m_Parent->GetBrowserId());
	if (!window) return;

	// refresh must be called by BHO thread
	window->PostMessage(BMM_REQUEST_INFO_ABOUT_PAGE, 0, 0);
}


void 
CConnectionManager::PerformRefresh()
{
	BrowserManagerLock browserManager;
	CBrowserMessageWindow* window = browserManager->FindBrowserMessageWindow(m_Parent->GetBrowserId());
	if (!window) return;

	// refresh must be called by BHO thread
	window->PostMessage(BMM_REQUEST_REFRESH, 0, 0);
}

void
CConnectionManager::Log(CString message, int icon)
{
	m_Parent->Log(message, icon);
}

void
CConnectionManager::Reconnect()
{
	Log("Reconnection request received", ICON_INFO);
	Disconnect();
	Connect();
}

void
CConnectionManager::StartReconnectListener()
{
	DWORD port = 41258;
	if (GetDWORDValueFromRegistry(REGISTRY_ROOT_KEY, REGISTRY_SETTINGS_KEY, REGISTRY_SETTINGS_PORT, &port)!=0)
	{
		Log(_T("Unable to read port from registry."), ICON_WARNING);
	}
	DWORD range = 16;
	if (GetDWORDValueFromRegistry(REGISTRY_ROOT_KEY, REGISTRY_SETTINGS_KEY, REGISTRY_SETTINGS_RANGE, &range)!=0)
	{
		Log(_T("Unable to read port range from registry."), ICON_WARNING);
	}
	for (unsigned int i=1; i<=range; i++)
	{
		CString p;
		p.Format(_T("%d"), port-i);
		m_ReconnectListener.SetSmartAddressing(false); 
		m_ReconnectListener.SetServerState(true); 
		if (m_ReconnectListener.CreateSocket(p, AF_INET, SOCK_STREAM))
		{
			m_ReconnectListener.WatchComm();
			return;
		}
	}
	TRACE_W(_T("Unable to assign port for reconnect listener"));
}

void
CConnectionManager::StopReconnectListener()
{
	m_ReconnectListener.StopComm();
}

void 
CConnectionManager::RequestStartingReconnectListener()
{
	BrowserManagerLock browserManager;
	CBrowserMessageWindow* window = browserManager->FindBrowserMessageWindow(m_Parent->GetBrowserId());
	if (!window) return;

	// refresh must be called by BHO thread
	window->PostMessage(BMM_REQUEST_LISTEN_FOR_RECONNECT, 0, 0);
}