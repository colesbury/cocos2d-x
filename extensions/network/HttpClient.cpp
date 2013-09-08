/****************************************************************************
 Copyright (c) 2010-2012 cocos2d-x.org
 Copyright (c) 2012 greathqy

 http://www.cocos2d-x.org

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 ****************************************************************************/

#include "HttpClient.h"
#include <thread>
#include <queue>
#include <errno.h>
#include <unistd.h>

#include "curl/curl.h"

#include "platform/CCFileUtils.h"

NS_CC_EXT_BEGIN

static std::mutex       s_requestQueueMutex;
static std::mutex       s_responseQueueMutex;

static unsigned long    s_asyncRequestCount = 0;

#if (CC_TARGET_PLATFORM == CC_PLATFORM_WIN32)
typedef int int32_t;
#endif

static bool s_need_quit = false;

static Array* s_requestQueue = NULL;
static int s_requestPipe = -1;
static Array* s_responseQueue = NULL;

static HttpClient *s_pHttpClient = NULL; // pointer to singleton

typedef size_t (*write_callback)(void *ptr, size_t size, size_t nmemb, void *stream);

static std::string s_cookieFilename = "";

// Callback function used by libcurl for collect response data
static size_t writeData(void *ptr, size_t size, size_t nmemb, void *stream)
{
    std::vector<char> *recvBuffer = (std::vector<char>*)stream;
    size_t sizes = size * nmemb;

    // add data to the end of recvBuffer
    // write data maybe called more than once in a single request
    recvBuffer->insert(recvBuffer->end(), (char*)ptr, (char*)ptr+sizes);

    return sizes;
}

// Callback function used by libcurl for collect header data
static size_t writeHeaderData(void *ptr, size_t size, size_t nmemb, void *stream)
{
    std::vector<char> *recvBuffer = (std::vector<char>*)stream;
    size_t sizes = size * nmemb;

    // add data to the end of recvBuffer
    // write data maybe called more than once in a single request
    recvBuffer->insert(recvBuffer->end(), (char*)ptr, (char*)ptr+sizes);

    return sizes;
}


static int processGetTask(HttpRequest *request, write_callback callback, void *stream, int32_t *errorCode, write_callback headerCallback, void *headerStream);
static int processPostTask(HttpRequest *request, write_callback callback, void *stream, int32_t *errorCode, write_callback headerCallback, void *headerStream);
static int processPutTask(HttpRequest *request, write_callback callback, void *stream, int32_t *errorCode, write_callback headerCallback, void *headerStream);
static int processDeleteTask(HttpRequest *request, write_callback callback, void *stream, int32_t *errorCode, write_callback headerCallback, void *headerStream);
// int processDownloadTask(HttpRequest *task, write_callback callback, void *stream, int32_t *errorCode);

static struct timeval get_curl_timeout(CURLM *multi_handle)
{
    /* set a suitable timeout to play around with */
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    long curl_timeo = -1;
    curl_multi_timeout(multi_handle, &curl_timeo);

    if(curl_timeo >= 0) {
      timeout.tv_sec = curl_timeo / 1000;
      if(timeout.tv_sec > 1)
        timeout.tv_sec = 1;
      else
        timeout.tv_usec = (curl_timeo % 1000) * 1000;
    }

    return timeout;
}

//Configure curl's timeout property
static bool configureCURL(CURL *handle, char* errorBuffer)
{
    if (!handle) {
        return false;
    }

    int32_t code;
    code = curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errorBuffer);
    if (code != CURLE_OK) {
        return false;
    }
    code = curl_easy_setopt(handle, CURLOPT_TIMEOUT, HttpClient::getInstance()->getTimeoutForRead());
    if (code != CURLE_OK) {
        return false;
    }
    code = curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, HttpClient::getInstance()->getTimeoutForConnect());
    if (code != CURLE_OK) {
        return false;
    }
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 0L);

    return true;
}

class CURLRaii
{
    /// Instance of CURL
    CURL *_curl;
    /// Keeps custom header data
    curl_slist *_headers;
    /// Error buffer
    char _errorBuffer[CURL_ERROR_SIZE];
    // HttpClient data
    HttpRequest *_request;
    HttpResponse *_response;
public:
    CURLRaii(HttpRequest *request)
        : _curl(curl_easy_init())
        , _headers(NULL)
        , _request(request)
        , _response(new HttpResponse(request))
    {
        CC_SAFE_RETAIN(_request);
    }

    ~CURLRaii()
    {
        if (_curl)
            curl_easy_cleanup(_curl);
        /* free the linked list for header data */
        if (_headers)
            curl_slist_free_all(_headers);
        CC_SAFE_RELEASE(_request);
        CC_SAFE_RELEASE(_response);
    }

    template <class T>
    bool setOption(CURLoption option, T data)
    {
        return CURLE_OK == curl_easy_setopt(_curl, option, data);
    }

    /**
     * @brief Inits CURL instance for common usage
     * @param request Null not allowed
     * @param callback Response write callback
     * @param stream Response write stream
     */
    bool init(HttpRequest *request, write_callback callback, void *stream, write_callback headerCallback, void *headerStream)
    {
        if (!_curl)
            return false;
        if (!configureCURL(_curl, _errorBuffer))
            return false;

        /* get custom header data (if set) */
       	std::vector<std::string> headers=request->getHeaders();
        if(!headers.empty())
        {
            /* append custom headers one by one */
            for (std::vector<std::string>::iterator it = headers.begin(); it != headers.end(); ++it)
                _headers = curl_slist_append(_headers,it->c_str());
            /* set custom headers for curl */
            if (!setOption(CURLOPT_HTTPHEADER, _headers))
                return false;
        }
        if (!s_cookieFilename.empty()) {
            if (!setOption(CURLOPT_COOKIEFILE, s_cookieFilename.c_str())) {
                return false;
            }
            if (!setOption(CURLOPT_COOKIEJAR, s_cookieFilename.c_str())) {
                return false;
            }
        }

        return setOption(CURLOPT_URL, request->getUrl())
                && setOption(CURLOPT_WRITEFUNCTION, callback)
                && setOption(CURLOPT_WRITEDATA, stream)
                && setOption(CURLOPT_HEADERFUNCTION, headerCallback)
                && setOption(CURLOPT_HEADERDATA, headerStream);

    }

    //Process Get Request
    bool initGet(HttpRequest *request, HttpResponse *response)
    {
        return init(request, writeData, response->getResponseData(), writeHeaderData, response->getResponseHeader())
            && setOption(CURLOPT_FOLLOWLOCATION, true);
    }

    //Process POST Request
    bool initPost(HttpRequest *request, HttpResponse *response)
    {
        return init(request, writeData, response->getResponseData(), writeHeaderData, response->getResponseHeader())
            && setOption(CURLOPT_POST, 1)
            && setOption(CURLOPT_POSTFIELDS, request->getRequestData())
            && setOption(CURLOPT_POSTFIELDSIZE, request->getRequestDataSize());
    }

    //Process PUT Request
    bool initPut(HttpRequest *request, HttpResponse *response)
    {
        return init(request, writeData, response->getResponseData(), writeHeaderData, response->getResponseHeader())
            && setOption(CURLOPT_CUSTOMREQUEST, "PUT")
            && setOption(CURLOPT_POSTFIELDS, request->getRequestData())
            && setOption(CURLOPT_POSTFIELDSIZE, request->getRequestDataSize());
    }

    //Process DELETE Request
    bool initDelete(HttpRequest *request, HttpResponse *response)
    {
        return init(request, writeData, response->getResponseData(), writeHeaderData, response->getResponseHeader())
            && setOption(CURLOPT_CUSTOMREQUEST, "DELETE")
            && setOption(CURLOPT_FOLLOWLOCATION, true);
    }

    bool init()
    {
        // Process the request -> get response packet
        switch (_request->getRequestType())
        {
            case HttpRequest::kHttpGet: // HTTP GET
                return initGet(_request, _response);
            case HttpRequest::kHttpPost: // HTTP POST
                return initPost(_request, _response);
            case HttpRequest::kHttpPut:
                return initPut(_request, _response);
            case HttpRequest::kHttpDelete:
                return initDelete(_request, _response);
            default:
                CCAssert(true, "CCHttpClient: unkown request type, only GET and POSt are supported");
                break;
        }
        return false;
    }

    bool attach(CURLM *multi_handle)
    {
        return CURLE_OK == curl_multi_add_handle(multi_handle, _curl);
    }

    bool processMsg(CURLM *multi_handle, CURLMsg *msg)
    {
        if (msg->easy_handle == _curl && msg->msg == CURLMSG_DONE) {
            curl_multi_remove_handle(multi_handle, _curl);

            if (msg->data.result != CURLE_OK) {
                _response->setSucceed(false);
                _response->setErrorBuffer(_errorBuffer);
            } else {
                _response->setSucceed(true);
            }

            long http_code = 0;
            curl_easy_getinfo(_curl, CURLINFO_RESPONSE_CODE, &http_code);

            _response->setResponseCode(http_code);
            return true;
        }
        return false;
    }

    HttpResponse* getResponse() { return _response; }
};

// Worker thread
static void networkThread(int pipe_fd)
{
    std::vector<std::unique_ptr<CURLRaii>> handles;
    CURLM *multi_handle = curl_multi_init();

    while (true)
    {
        if (s_need_quit)
        {
            break;
        }

        struct timeval timeout = get_curl_timeout(multi_handle);

        int maxfd = -1;
        fd_set fdread;
        fd_set fdwrite;
        fd_set fdexcep;
        FD_ZERO(&fdread);
        FD_ZERO(&fdwrite);
        FD_ZERO(&fdexcep);

        /* get file descriptors from the transfers */
        curl_multi_fdset(multi_handle, &fdread, &fdwrite, &fdexcep, &maxfd);

        /* add the pipe */
        FD_SET(pipe_fd, &fdread);
        if (pipe_fd > maxfd) {
            maxfd = pipe_fd;
        }

        /* In a real-world program you OF COURSE check the return code of the
           function calls.  On success, the value of maxfd is guaranteed to be
           greater or equal than -1.  We call select(maxfd + 1, ...), specially in
           case of (maxfd == -1), we call select(0, ...), which is basically equal
           to sleep. */
        int rc = select(maxfd+1, &fdread, &fdwrite, &fdexcep, &timeout);
        if (rc < 0) {
            // HANDLE
            break;
        }

        if (FD_ISSET(pipe_fd, &fdread)) {
            HttpRequest *request = NULL;

            char token;
            read(pipe_fd, &token, 1);

            s_requestQueueMutex.lock();

            //Get request task from queue
            if (0 != s_requestQueue->count())
            {
                request = dynamic_cast<HttpRequest*>(s_requestQueue->objectAtIndex(0));
                s_requestQueue->removeObjectAtIndex(0);
            }

            s_requestQueueMutex.unlock();

            if (request) {
                handles.emplace_back(new CURLRaii(request));
                handles.back()->init();
                handles.back()->attach(multi_handle);

                // request's refcount = 2 here, it's retained by CURLRaii constructor
                request->release();
            }
        }

        // Perform the cURL requests
        int still_running;
        curl_multi_perform(multi_handle, &still_running);

        // Check for completed requests
        CURLMsg *msg = NULL;
        int msgs_in_queue;
        while ((msg = curl_multi_info_read(multi_handle, &msgs_in_queue)) != NULL)
        {
            for (auto it = handles.begin(); it != handles.end(); ++it) {
                CURLRaii *curl = it->get();
                if (curl->processMsg(multi_handle, msg)) {
                    HttpResponse *response = curl->getResponse();
                    response->retain();

                    // add response packet into queue
                    s_responseQueueMutex.lock();
                    s_responseQueue->addObject(response);
                    s_responseQueueMutex.unlock();

                    handles.erase(it);
                    break;
                }
            }
        }
    }

    // cleanup: if worker thread received quit signal, clean up un-completed request queue
    s_requestQueueMutex.lock();
    s_requestQueue->removeAllObjects();
    s_requestQueueMutex.unlock();

    s_asyncRequestCount -= s_requestQueue->count();

    if (s_requestQueue != NULL) {

        s_requestQueue->release();
        s_requestQueue = NULL;
        s_responseQueue->release();
        s_responseQueue = NULL;
    }
}

// HttpClient implementation
HttpClient* HttpClient::getInstance()
{
    if (s_pHttpClient == NULL) {
        s_pHttpClient = new HttpClient();
    }

    return s_pHttpClient;
}

void HttpClient::destroyInstance()
{
    CCAssert(s_pHttpClient, "");
    Director::getInstance()->getScheduler()->unscheduleSelector(schedule_selector(HttpClient::dispatchResponseCallbacks), s_pHttpClient);
    s_pHttpClient->release();
}

void HttpClient::enableCookies(const char* cookieFile) {
    if (cookieFile) {
        s_cookieFilename = std::string(cookieFile);
    }
    else {
        s_cookieFilename = (FileUtils::getInstance()->getWritablePath() + "cookieFile.txt");
    }
}

HttpClient::HttpClient()
: _timeoutForConnect(30)
, _timeoutForRead(60)
{
    Director::getInstance()->getScheduler()->scheduleSelector(
                    schedule_selector(HttpClient::dispatchResponseCallbacks), this, 0, false);
    Director::getInstance()->getScheduler()->pauseTarget(this);
}

HttpClient::~HttpClient()
{
    s_need_quit = true;

    if (s_requestQueue != NULL) {
        char token = 0;
        write(s_requestPipe, &token, 1);
    }

    s_pHttpClient = NULL;
}

//Lazy create semaphore & mutex & thread
bool HttpClient::lazyInitThreadSemphore()
{
    if (s_requestQueue != NULL) {
        return true;
    } else {
        int fds[2];
        pipe(fds);

        s_requestPipe = fds[1];

        s_requestQueue = new Array();
        s_requestQueue->init();

        s_responseQueue = new Array();
        s_responseQueue->init();

        auto t = std::thread(&networkThread, fds[0]);
        t.detach();

        s_need_quit = false;
    }

    return true;
}

//Add a get task to queue
void HttpClient::send(HttpRequest* request)
{
    if (false == lazyInitThreadSemphore())
    {
        return;
    }

    if (!request)
    {
        return;
    }

    ++s_asyncRequestCount;

    request->retain();

    s_requestQueueMutex.lock();
    s_requestQueue->addObject(request);
    s_requestQueueMutex.unlock();

    // Notify thread start to work
    char token = 0;
    write(s_requestPipe, &token, 1);

    // resume dispatcher selector
    Director::getInstance()->getScheduler()->resumeTarget(this);
}

// Poll and notify main thread if responses exists in queue
void HttpClient::dispatchResponseCallbacks(float delta)
{
    // CCLog("CCHttpClient::dispatchResponseCallbacks is running");

    HttpResponse* response = NULL;

    s_responseQueueMutex.lock();

    if (s_responseQueue->count())
    {
        response = dynamic_cast<HttpResponse*>(s_responseQueue->objectAtIndex(0));
        s_responseQueue->removeObjectAtIndex(0);
    }

    s_responseQueueMutex.unlock();

    if (response)
    {
        --s_asyncRequestCount;

        HttpRequest *request = response->getHttpRequest();
        Object *pTarget = request->getTarget();
        SEL_HttpResponse pSelector = request->getSelector();

        if (pTarget && pSelector)
        {
            (pTarget->*pSelector)(this, response);
        }

        response->release();
    }

    if (0 == s_asyncRequestCount)
    {
        Director::getInstance()->getScheduler()->pauseTarget(this);
    }

}

NS_CC_EXT_END


