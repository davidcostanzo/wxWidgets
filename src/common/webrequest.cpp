///////////////////////////////////////////////////////////////////////////////
// Name:        src/common/webrequest.cpp
// Purpose:     wxWebRequest base class implementations
// Author:      Tobias Taschner
// Created:     2018-10-17
// Copyright:   (c) 2018 wxWidgets development team
// Licence:     wxWindows licence
///////////////////////////////////////////////////////////////////////////////

// For compilers that support precompilation, includes "wx.h".
#include "wx/wxprec.h"

#if wxUSE_WEBREQUEST

#include "wx/webrequest.h"
#include "wx/mstream.h"
#include "wx/module.h"
#include "wx/uri.h"
#include "wx/filefn.h"
#include "wx/filename.h"
#include "wx/stdpaths.h"
#include "wx/wfstream.h"

#ifndef WX_PRECOMP
    #include "wx/app.h"
    #include "wx/translation.h"
    #include "wx/utils.h"
#endif

#include "wx/private/webrequest.h"

#if wxUSE_WEBREQUEST_WINHTTP
#include "wx/msw/private/webrequest_winhttp.h"
#endif
#if wxUSE_WEBREQUEST_URLSESSION
#include "wx/osx/private/webrequest_urlsession.h"
#endif
#if wxUSE_WEBREQUEST_CURL
#include "wx/private/webrequest_curl.h"
#endif

extern WXDLLIMPEXP_DATA_NET(const char) wxWebSessionBackendWinHTTP[] = "wxWebSessionBackendWinHTTP";
extern WXDLLIMPEXP_DATA_NET(const char) wxWebSessionBackendURLSession[] = "wxWebSessionBackendURLSession";
extern WXDLLIMPEXP_DATA_NET(const char) wxWebSessionBackendCURL[] = "wxWebSessionBackendCURL";

wxDEFINE_EVENT(wxEVT_WEBREQUEST_STATE, wxWebRequestEvent);
wxDEFINE_EVENT(wxEVT_WEBREQUEST_DATA, wxWebRequestEvent);

static const wxStringCharType* wxNO_IMPL_MSG
    = wxS("can't be used with an invalid/uninitialized object");

#define wxCHECK_IMPL(rc) wxCHECK_MSG( m_impl, (rc), wxNO_IMPL_MSG )
#define wxCHECK_IMPL_VOID() wxCHECK_RET( m_impl, wxNO_IMPL_MSG )

//
// wxWebRequestImpl
//
wxWebRequestImpl::wxWebRequestImpl(wxWebSession& session, wxEvtHandler* handler, int id)
    : m_storage(wxWebRequest::Storage_Memory),
      m_headers(session.GetImpl()->GetHeaders()),
      m_dataSize(0),
      m_session(session),
      m_handler(handler),
      m_id(id),
      m_state(wxWebRequest::State_Idle),
      m_ignoreServerErrorStatus(false),
      m_bytesReceived(0)
{
}

bool wxWebRequestImpl::CheckServerStatus()
{
    const wxWebResponseImplPtr& resp = GetResponse();
    if ( resp && resp->GetStatus() >= 400 && !m_ignoreServerErrorStatus )
    {
        SetState(wxWebRequest::State_Failed, wxString::Format(_("Error: %s (%d)"),
            resp->GetStatusText(), resp->GetStatus()));
        return false;
    }
    else
        return true;
}

bool wxWebRequestImpl::IsActiveState(wxWebRequest::State state)
{
    return (state == wxWebRequest::State_Active || state == wxWebRequest::State_Unauthorized);
}

void wxWebRequestImpl::SetData(const wxString& text, const wxString& contentType, const wxMBConv& conv)
{
    m_dataText = text.mb_str(conv);

    wxScopedPtr<wxInputStream>
        stream(new wxMemoryInputStream(m_dataText, m_dataText.length()));
    SetData(stream, contentType);
}

bool
wxWebRequestImpl::SetData(wxScopedPtr<wxInputStream>& dataStream,
                          const wxString& contentType,
                          wxFileOffset dataSize)
{
    m_dataStream.reset(dataStream.release());

    if ( m_dataStream )
    {
        wxCHECK_MSG( m_dataStream->IsOk(), false, "can't use invalid stream" );

        if ( dataSize == wxInvalidOffset )
        {
            // Determine data size
            m_dataSize = m_dataStream->SeekI(0, wxFromEnd);
            if ( m_dataSize == wxInvalidOffset )
                return false;

            m_dataStream->SeekI(0);
        }
        else
            m_dataSize = dataSize;
    }
    else
        m_dataSize = 0;

    SetHeader("Content-Type", contentType);

    return true;
}

wxFileOffset wxWebRequestImpl::GetBytesReceived() const
{
    return m_bytesReceived;
}

wxFileOffset wxWebRequestImpl::GetBytesExpectedToReceive() const
{
    if ( GetResponse() )
        return GetResponse()->GetContentLength();
    else
        return -1;
}

namespace
{

// Functor used with CallAfter() below.
//
// TODO-C++11: Replace with a lambda.
struct StateEventProcessor
{
    StateEventProcessor(wxWebRequestImpl& request,
                        wxWebRequest::State state,
                        const wxString& failMsg)
        : m_request(request), m_state(state), m_failMsg(failMsg)
    {
        // Ensure that the request object stays alive until this event is
        // processed.
        m_request.IncRef();
    }

    StateEventProcessor(const StateEventProcessor& other)
        : m_request(other.m_request), m_state(other.m_state), m_failMsg(other.m_failMsg)
    {
        m_request.IncRef();
    }

    void operator()()
    {
        m_request.ProcessStateEvent(m_state, m_failMsg);
    }

    ~StateEventProcessor()
    {
        m_request.DecRef();
    }

    wxWebRequestImpl& m_request;
    const wxWebRequest::State m_state;
    const wxString m_failMsg;
};

} // anonymous namespace

void wxWebRequestImpl::SetState(wxWebRequest::State state, const wxString & failMsg)
{
    m_state = state;

    // Trigger the event in the main thread
    m_handler->CallAfter(StateEventProcessor(*this, state, failMsg));
}

void wxWebRequestImpl::ReportDataReceived(size_t sizeReceived)
{
    m_bytesReceived += sizeReceived;
}

// The SplitParamaters implementation is adapted to wxWidgets
// from Poco::Net::MessageHeader::splitParameters

// This function is used in a unit test, so define it inside wxPrivate
// namespace and an anonymous one.
namespace wxPrivate
{

WXDLLIMPEXP_NET wxString
SplitParameters(const wxString& s, wxWebRequestHeaderMap& parameters)
{
    wxString value;
    wxString::const_iterator it = s.begin();
    wxString::const_iterator end = s.end();
    while ( it != end && wxIsspace(*it) )
        ++it;
    while ( it != end && *it != ';' )
        value += *it++;
    value.Trim();
    if ( it != end )
        ++it;

    parameters.clear();
    wxString pname;
    wxString pvalue;
    pname.reserve(32);
    pvalue.reserve(64);
    while ( it != end )
    {
        pname.clear();
        pvalue.clear();
        while ( it != end && wxIsspace(*it) )
            ++it;
        while ( it != end && *it != '=' && *it != ';' )
            pname += *it++;
        pname.Trim();
        if ( it != end && *it != ';' )
            ++it;
        while ( it != end && wxIsspace(*it) )
            ++it;
        while ( it != end && *it != ';' )
        {
            if ( *it == '"' )
            {
                ++it;
                while ( it != end && *it != '"' )
                {
                    if ( *it == '\\' )
                    {
                        ++it;
                        if ( it != end )
                            pvalue += *it++;
                    }
                    else
                        pvalue += *it++;
                }
                if ( it != end )
                    ++it;
            }
            else if ( *it == '\\' )
            {
                ++it;
                if ( it != end )
                    pvalue += *it++;
            }
            else
                pvalue += *it++;
        }
        pvalue.Trim();
        if ( !pname.empty() )
            parameters[pname] = pvalue;
        if ( it != end )
            ++it;
    }

    return value;
}

} // namespace wxPrivate

void wxWebRequestImpl::ProcessStateEvent(wxWebRequest::State state, const wxString& failMsg)
{
    if ( !IsActiveState(state) && GetResponse() )
        GetResponse()->Finalize();

    wxString responseFileName;

    wxWebRequestEvent evt(wxEVT_WEBREQUEST_STATE, GetId(), state,
        wxWebResponse(GetResponse()), failMsg);
    if ( state == wxWebRequest::State_Completed && m_storage == wxWebRequest::Storage_File )
    {
        responseFileName = GetResponse()->GetFileName();
        evt.SetResponseFileName(responseFileName);
    }

    m_handler->ProcessEvent(evt);

    // Remove temporary file if it still exists
    if ( state == wxWebRequest::State_Completed && m_storage == wxWebRequest::Storage_File &&
        wxFileExists(responseFileName) )
        wxRemoveFile(responseFileName);
}

//
// wxWebRequest
//

wxWebRequest::wxWebRequest()
{
}

wxWebRequest::wxWebRequest(const wxWebRequestImplPtr& impl)
    : m_impl(impl)
{
}

wxWebRequest::wxWebRequest(const wxWebRequest& other)
    : m_impl(other.m_impl)
{
}

wxWebRequest& wxWebRequest::operator=(const wxWebRequest& other)
{
    m_impl = other.m_impl;
    return *this;
}

wxWebRequest::~wxWebRequest()
{
}

void wxWebRequest::SetHeader(const wxString& name, const wxString& value)
{
    wxCHECK_IMPL_VOID();

    m_impl->SetHeader(name, value);
}

void wxWebRequest::SetMethod(const wxString& method)
{
    wxCHECK_IMPL_VOID();

    m_impl->SetMethod(method);
}

void wxWebRequest::SetData(const wxString& text, const wxString& contentType, const wxMBConv& conv)
{
    wxCHECK_IMPL_VOID();

    m_impl->SetData(text, contentType, conv);
}

bool
wxWebRequest::SetData(wxInputStream* dataStream,
                      const wxString& contentType,
                      wxFileOffset dataSize)
{
    // Ensure that the stream is destroyed even we return below.
    wxScopedPtr<wxInputStream> streamPtr(dataStream);

    wxCHECK_IMPL( false );

    return m_impl->SetData(streamPtr, contentType, dataSize);
}

void wxWebRequest::SetIgnoreServerErrorStatus(bool ignore)
{
    wxCHECK_IMPL_VOID();

    m_impl->SetIgnoreServerErrorStatus(ignore);
}

void wxWebRequest::SetStorage(Storage storage)
{
    wxCHECK_IMPL_VOID();

    m_impl->SetStorage(storage);
}

wxWebRequest::Storage wxWebRequest::GetStorage() const
{
    wxCHECK_IMPL( Storage_None );

    return m_impl->GetStorage();
}

void wxWebRequest::Start()
{
    wxCHECK_IMPL_VOID();

    wxCHECK_RET( m_impl->GetState() == wxWebRequest::State_Idle,
                 "Completed requests can not be restarted" );

    m_impl->Start();
}

void wxWebRequest::Cancel()
{
    wxCHECK_IMPL_VOID();

    m_impl->Cancel();
}

wxWebResponse wxWebRequest::GetResponse() const
{
    wxCHECK_IMPL( wxWebResponse() );

    return wxWebResponse(m_impl->GetResponse());
}

wxWebAuthChallenge wxWebRequest::GetAuthChallenge() const
{
    wxCHECK_IMPL( wxWebAuthChallenge() );

    return wxWebAuthChallenge(m_impl->GetAuthChallenge());
}

int wxWebRequest::GetId() const
{
    wxCHECK_IMPL( wxID_ANY );

    return m_impl->GetId();
}

wxWebSession& wxWebRequest::GetSession() const
{
    wxCHECK_IMPL( wxWebSession::GetDefault() );

    return m_impl->GetSession();
}

wxWebRequest::State wxWebRequest::GetState() const
{
    wxCHECK_IMPL( State_Failed );

    return m_impl->GetState();
}

wxFileOffset wxWebRequest::GetBytesSent() const
{
    wxCHECK_IMPL( wxInvalidOffset );

    return m_impl->GetBytesSent();
}

wxFileOffset wxWebRequest::GetBytesExpectedToSend() const
{
    wxCHECK_IMPL( wxInvalidOffset );

    return m_impl->GetBytesExpectedToSend();
}

wxFileOffset wxWebRequest::GetBytesReceived() const
{
    wxCHECK_IMPL( wxInvalidOffset );

    return m_impl->GetBytesReceived();
}

wxFileOffset wxWebRequest::GetBytesExpectedToReceive() const
{
    wxCHECK_IMPL( wxInvalidOffset );

    return m_impl->GetBytesExpectedToReceive();
}


//
// wxWebAuthChallenge
//

wxWebAuthChallenge::wxWebAuthChallenge()
{
}

wxWebAuthChallenge::wxWebAuthChallenge(const wxWebAuthChallengeImplPtr& impl)
    : m_impl(impl)
{
}

wxWebAuthChallenge::wxWebAuthChallenge(const wxWebAuthChallenge& other)
    : m_impl(other.m_impl)
{
}

wxWebAuthChallenge& wxWebAuthChallenge::operator=(const wxWebAuthChallenge& other)
{
    m_impl = other.m_impl;
    return *this;
}

wxWebAuthChallenge::~wxWebAuthChallenge()
{
}

wxWebAuthChallenge::Source wxWebAuthChallenge::GetSource() const
{
    wxCHECK_IMPL( Source_Server );

    return m_impl->GetSource();
}

void
wxWebAuthChallenge::SetCredentials(const wxWebCredentials& cred)
{
    wxCHECK_IMPL_VOID();

    m_impl->SetCredentials(cred);
}

//
// wxWebResponseImpl
//

wxWebResponseImpl::wxWebResponseImpl(wxWebRequestImpl& request) :
    m_request(request),
    m_readSize(8 * 1024)
{
}

wxWebResponseImpl::~wxWebResponseImpl()
{
    if ( wxFileExists(m_file.GetName()) )
        wxRemoveFile(m_file.GetName());
}

void wxWebResponseImpl::Init()
{
    if ( m_request.GetStorage() == wxWebRequest::Storage_File )
    {
        wxFileName tmpPrefix;
        tmpPrefix.AssignDir(m_request.GetSession().GetTempDir());
        if ( GetContentLength() > 0 )
        {
            // Check available disk space
            wxLongLong freeSpace;
            if ( wxGetDiskSpace(tmpPrefix.GetFullPath(), NULL, &freeSpace) &&
                GetContentLength() > freeSpace )
            {
                m_request.SetState(wxWebRequest::State_Failed, _("Not enough free disk space for download."));
                return;
            }
        }

        tmpPrefix.SetName("wxd");
        wxFileName::CreateTempFileName(tmpPrefix.GetFullPath(), &m_file);
    }
}

wxString wxWebResponseImpl::GetMimeType() const
{
    return GetHeader("Mime-Type");
}

wxInputStream * wxWebResponseImpl::GetStream() const
{
    if ( !m_stream.get() )
    {
        // Create stream
        switch ( m_request.GetStorage() )
        {
            case wxWebRequest::Storage_Memory:
                m_stream.reset(new wxMemoryInputStream(m_readBuffer.GetData(), m_readBuffer.GetDataLen()));
                break;
            case wxWebRequest::Storage_File:
                m_stream.reset(new wxFFileInputStream(m_file));
                m_stream->SeekI(0);
                break;
            case wxWebRequest::Storage_None:
                // No stream available
                break;
        }

    }

    return m_stream.get();
}

wxString wxWebResponseImpl::GetSuggestedFileName() const
{
    wxString suggestedFilename;

    // Try to determine from Content-Disposition header
    wxString contentDisp = GetHeader("Content-Disposition");
    wxWebRequestHeaderMap params;
    const wxString disp = wxPrivate::SplitParameters(contentDisp, params);
    if ( disp == "attachment" )
    {
        // Parse as filename to filter potential path names
        wxFileName fn(params["filename"]);
        suggestedFilename = fn.GetFullName();
    }

    if ( suggestedFilename.empty() )
    {
        wxURI uri(GetURL());
        if ( uri.HasPath() )
        {
            wxFileName fn(uri.GetPath());
            suggestedFilename = fn.GetFullName();
        }
        else
            suggestedFilename = uri.GetServer();
    }

    return suggestedFilename;
}

wxString wxWebResponseImpl::AsString() const
{
    if ( m_request.GetStorage() == wxWebRequest::Storage_Memory )
    {
        // TODO: try to determine encoding type from content-type header
        size_t outLen = 0;
        return wxConvWhateverWorks.cMB2WC((const char*)m_readBuffer.GetData(), m_readBuffer.GetDataLen(), &outLen);
    }
    else
        return wxString();
}

void* wxWebResponseImpl::GetDataBuffer(size_t sizeNeeded)
{
    return m_readBuffer.GetAppendBuf(sizeNeeded);
}

void wxWebResponseImpl::ReportDataReceived(size_t sizeReceived)
{
    m_readBuffer.UngetAppendBuf(sizeReceived);
    m_request.ReportDataReceived(sizeReceived);

    if ( m_request.GetStorage() == wxWebRequest::Storage_File )
    {
        m_file.Write(m_readBuffer.GetData(), m_readBuffer.GetDataLen());
    }
    else if ( m_request.GetStorage() == wxWebRequest::Storage_None )
    {
        wxWebRequestEvent evt(wxEVT_WEBREQUEST_DATA, m_request.GetId(), wxWebRequest::State_Active);
        evt.SetDataBuffer(m_readBuffer.GetData(), m_readBuffer.GetDataLen());
        m_request.GetHandler()->ProcessEvent(evt);
    }

    if ( m_request.GetStorage() != wxWebRequest::Storage_Memory )
        m_readBuffer.Clear();
}

wxString wxWebResponseImpl::GetFileName() const
{
    return m_file.GetName();
}

void wxWebResponseImpl::Finalize()
{
    if ( m_request.GetStorage() == wxWebRequest::Storage_File )
        m_file.Close();
}

//
// wxWebResponse
//

wxWebResponse::wxWebResponse()
{
}

wxWebResponse::wxWebResponse(const wxWebResponseImplPtr& impl)
    : m_impl(impl)
{
}

wxWebResponse::wxWebResponse(const wxWebResponse& other)
    : m_impl(other.m_impl)
{
}

wxWebResponse& wxWebResponse::operator=(const wxWebResponse& other)
{
    m_impl = other.m_impl;
    return *this;
}

wxWebResponse::~wxWebResponse()
{
}

wxInt64 wxWebResponse::GetContentLength() const
{
    wxCHECK_IMPL( -1 );

    return m_impl->GetContentLength();
}

wxString wxWebResponse::GetURL() const
{
    wxCHECK_IMPL( wxString() );

    return m_impl->GetURL();
}

wxString wxWebResponse::GetHeader(const wxString& name) const
{
    wxCHECK_IMPL( wxString() );

    return m_impl->GetHeader(name);
}

wxString wxWebResponse::GetMimeType() const
{
    wxCHECK_IMPL( wxString() );

    return m_impl->GetMimeType();
}

int wxWebResponse::GetStatus() const
{
    wxCHECK_IMPL( -1 );

    return m_impl->GetStatus();
}

wxString wxWebResponse::GetStatusText() const
{
    wxCHECK_IMPL( wxString() );

    return m_impl->GetStatusText();
}

wxInputStream* wxWebResponse::GetStream() const
{
    wxCHECK_IMPL( NULL );

    return m_impl->GetStream();
}

wxString wxWebResponse::GetSuggestedFileName() const
{
    wxCHECK_IMPL( wxString() );

    return m_impl->GetSuggestedFileName();
}

wxString wxWebResponse::AsString() const
{
    wxCHECK_IMPL( wxString() );

    return m_impl->AsString();
}

wxString wxWebResponse::GetFileName() const
{
    wxCHECK_IMPL( wxString() );

    return m_impl->GetFileName();
}


//
// wxWebSessionImpl
//

WX_DECLARE_STRING_HASH_MAP(wxWebSessionFactory*, wxStringWebSessionFactoryMap);

namespace
{

wxWebSession gs_defaultSession;
wxStringWebSessionFactoryMap gs_factoryMap;

} // anonymous namespace

wxWebSessionImpl::wxWebSessionImpl()
{
    // Initialize the user-Agent header with a reasonable default
    AddCommonHeader("User-Agent", wxString::Format("%s/1 wxWidgets/%d.%d.%d",
        wxTheApp->GetAppName(),
        wxMAJOR_VERSION, wxMINOR_VERSION, wxRELEASE_NUMBER));
}

wxString wxWebSessionImpl::GetTempDir() const
{
    if ( m_tempDir.empty() )
        return wxStandardPaths::Get().GetTempDir();
    else
        return m_tempDir;
}

//
// wxWebSession
//

wxWebSession::wxWebSession()
{
}

wxWebSession::wxWebSession(const wxWebSessionImplPtr& impl)
    : m_impl(impl)
{
}

wxWebSession::wxWebSession(const wxWebSession& other)
    : m_impl(other.m_impl)
{
}

wxWebSession& wxWebSession::operator=(const wxWebSession& other)
{
    m_impl = other.m_impl;
    return *this;
}

wxWebSession::~wxWebSession()
{
}

// static
wxWebSession& wxWebSession::GetDefault()
{
    if ( !gs_defaultSession.IsOpened() )
        gs_defaultSession = wxWebSession::New();

    return gs_defaultSession;
}

// static
wxWebSession wxWebSession::New(const wxString& backendOrig)
{
    if ( gs_factoryMap.empty() )
        InitFactoryMap();

    wxString backend = backendOrig;
    if ( backend.empty() )
    {
#if wxUSE_WEBREQUEST_WINHTTP
        backend = wxWebSessionBackendWinHTTP;
#elif wxUSE_WEBREQUEST_URLSESSION
        backend = wxWebSessionBackendURLSession;
#elif wxUSE_WEBREQUEST_CURL
        backend = wxWebSessionBackendCURL;
#endif
    }

    wxStringWebSessionFactoryMap::iterator factory = gs_factoryMap.find(backend);

    wxWebSessionImplPtr impl;
    if ( factory != gs_factoryMap.end() )
        impl = factory->second->Create();

    return wxWebSession(impl);
}

// static
void
wxWebSession::RegisterFactory(const wxString& backend,
                              wxWebSessionFactory* factory)
{
    // Note that we don't have to check here that there is no registered
    // backend with the same name yet because we're only called from
    // InitFactoryMap() below. If this function becomes public, we'd need to
    // free the previous pointer stored for this backend first here.
    gs_factoryMap[backend] = factory;
}

// static
void wxWebSession::InitFactoryMap()
{
#if wxUSE_WEBREQUEST_WINHTTP
    RegisterFactory(wxWebSessionBackendWinHTTP, new wxWebSessionFactoryWinHTTP());
#endif
#if wxUSE_WEBREQUEST_URLSESSION
    RegisterFactory(wxWebSessionBackendURLSession, new wxWebSessionFactoryURLSession());
#endif
#if wxUSE_WEBREQUEST_CURL
    RegisterFactory(wxWebSessionBackendCURL, new wxWebSessionFactoryCURL());
#endif
}

// static
bool wxWebSession::IsBackendAvailable(const wxString& backend)
{
    if ( gs_factoryMap.empty() )
        InitFactoryMap();

    wxStringWebSessionFactoryMap::iterator factory = gs_factoryMap.find(backend);
    return factory != gs_factoryMap.end();
}

wxWebRequest
wxWebSession::CreateRequest(wxEvtHandler* handler, const wxString& url, int id)
{
    wxCHECK_IMPL( wxWebRequest() );

    return wxWebRequest(m_impl->CreateRequest(*this, handler, url, id));
}

wxVersionInfo wxWebSession::GetLibraryVersionInfo()
{
    wxCHECK_IMPL( wxVersionInfo() );

    return m_impl->GetLibraryVersionInfo();
}

void wxWebSession::AddCommonHeader(const wxString& name, const wxString& value)
{
    wxCHECK_IMPL_VOID();

    m_impl->AddCommonHeader(name, value);
}

void wxWebSession::SetTempDir(const wxString& dir)
{
    wxCHECK_IMPL_VOID();

    m_impl->SetTempDir(dir);
}

wxString wxWebSession::GetTempDir() const
{
    wxCHECK_IMPL( wxString() );

    return m_impl->GetTempDir();
}

bool wxWebSession::IsOpened() const
{
    return m_impl.get() != NULL;
}

void wxWebSession::Close()
{
    m_impl.reset(NULL);
}

// ----------------------------------------------------------------------------
// Module ensuring all global/singleton objects are destroyed on shutdown.
// ----------------------------------------------------------------------------

class WebRequestModule : public wxModule
{
public:
    WebRequestModule()
    {
    }

    virtual bool OnInit() wxOVERRIDE
    {
        return true;
    }

    virtual void OnExit() wxOVERRIDE
    {
        for ( wxStringWebSessionFactoryMap::iterator it = gs_factoryMap.begin();
              it != gs_factoryMap.end();
              ++it )
        {
            delete it->second;
        }

        gs_factoryMap.clear();
        gs_defaultSession.Close();
    }

private:
    wxDECLARE_DYNAMIC_CLASS(WebRequestModule);
};

wxIMPLEMENT_DYNAMIC_CLASS(WebRequestModule, wxModule);

#endif // wxUSE_WEBREQUEST
