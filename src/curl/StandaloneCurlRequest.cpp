/*
 * This File is part of Davix, The IO library for HTTP based protocols
 * Copyright (C) CERN 2019
 * Author: Georgios Bitzes <georgois.bitzes@cern.ch>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
*/

#include "StandaloneCurlRequest.hpp"
#include "CurlSessionFactory.hpp"
#include "CurlSession.hpp"
#include "HeaderlineParser.hpp"
#include <utils/davix_logger_internal.hpp>
#include <core/ContentProvider.hpp>
#include <curl/curl.h>

#define SSTR(message) static_cast<std::ostringstream&>(std::ostringstream().flush() << message).str()
#define DBG(message) std::cerr << __FILE__ << ":" << __LINE__ << " -- " << #message << " = " << message << std::endl;

namespace Davix {

//------------------------------------------------------------------------------
// Header callback
//------------------------------------------------------------------------------
static size_t header_callback(char *buffer, size_t size, size_t nitems, void *userdata) {
  size_t bytes = size * nitems;

  StandaloneCurlRequest* req = (StandaloneCurlRequest*) userdata;
  req->feedResponseHeader(std::string(buffer, bytes));
  return bytes;
}

//------------------------------------------------------------------------------
// Write callback
//------------------------------------------------------------------------------
static size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
  size_t bytes = size * nmemb;

  ResponseBuffer* buff = (ResponseBuffer*) userdata;
  buff->feed(ptr, bytes);
  return bytes;
}

//------------------------------------------------------------------------------
// Read callback
//------------------------------------------------------------------------------
size_t read_callback(char *buffer, size_t size, size_t nitems, void *userdata) {
  size_t bytes = size * nitems;

  ContentProvider* provider = (ContentProvider*) userdata;
  ssize_t retval = provider->pullBytes(buffer, bytes);

  if(retval < 0) {
    DAVIX_SLOG(DAVIX_LOG_WARNING, DAVIX_LOG_HTTP, "Content provider reported an errc={}", retval);
    return 0;
  }

  return retval;
}

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
StandaloneCurlRequest::StandaloneCurlRequest(CurlSessionFactory &sessionFactory, bool reuseSession,
  const BoundHooks &boundHooks, const Uri &uri, const std::string &verb, const RequestParams &params,
  const std::vector<HeaderLine> &headers, int reqFlag, ContentProvider *contentProvider,
  Chrono::TimePoint deadline)
: _session_factory(sessionFactory), _reuse_session(reuseSession), _bound_hooks(boundHooks),
  _uri(uri), _verb(verb), _params(params), _headers(headers), _req_flag(reqFlag),
  _content_provider(contentProvider), _deadline(deadline), _state(RequestState::kNotStarted),
  _chunklist(NULL), _received_headers(false) {}

//------------------------------------------------------------------------------
// Destructor
//------------------------------------------------------------------------------
StandaloneCurlRequest::~StandaloneCurlRequest() {
    curl_slist_free_all(_chunklist);
}

//------------------------------------------------------------------------------
// Get a specific response header
//------------------------------------------------------------------------------
bool StandaloneCurlRequest::getAnswerHeader(const std::string &header_name, std::string &value) const {
  for(auto it = _response_headers.begin(); it != _response_headers.end(); it++) {
    if(it->first == header_name) {
      value = it->second;
      return true;
    }
  }

  return false;
}

//------------------------------------------------------------------------------
// Get all response headers
//------------------------------------------------------------------------------
size_t StandaloneCurlRequest::getAnswerHeaders(std::vector<std::pair<std::string, std::string > > & vec_headers) const {
  vec_headers = _response_headers;
  return vec_headers.size();
}

//------------------------------------------------------------------------------
// Start request - calling this multiple times will do nothing.
//------------------------------------------------------------------------------
Status StandaloneCurlRequest::startRequest() {
  if(_state != RequestState::kNotStarted) {
    return Status(); ;
  }

  //----------------------------------------------------------------------------
  // Have we timed out already?
  //----------------------------------------------------------------------------
  Status st = checkTimeout();
  if(!st.ok()) {
    // markCompleted();
    return st;
  }

  //----------------------------------------------------------------------------
  // Retrieve a session, create request
  //----------------------------------------------------------------------------
  _session = _session_factory.provideCurlSession(_uri, _params, st);
  if(!st.ok()) {
    // markCompleted();
    return st;
  }

  //----------------------------------------------------------------------------
  // Set request verb, target URL
  //----------------------------------------------------------------------------
  CURL* handle = _session->getHandle()->handle;
  CURLM* mhandle = _session->getHandle()->mhandle;

  curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, _verb.c_str());
  curl_easy_setopt(handle, CURLOPT_URL, _uri.getString().c_str());

  //----------------------------------------------------------------------------
  // Set up callback to consume response headers
  //----------------------------------------------------------------------------
  curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, header_callback);
  curl_easy_setopt(handle, CURLOPT_HEADERDATA, this);

  //----------------------------------------------------------------------------
  // Set up callback to consume response body
  //----------------------------------------------------------------------------
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, &_response_buffer);

  //----------------------------------------------------------------------------
  // Set up callback to provide request body
  //----------------------------------------------------------------------------
  if(_content_provider) {
    _content_provider->rewind();
    curl_easy_setopt(handle, CURLOPT_READFUNCTION, read_callback);
    curl_easy_setopt(handle, CURLOPT_READDATA, _content_provider);
  }

  //----------------------------------------------------------------------------
  // Set-up headers
  //----------------------------------------------------------------------------
  for(size_t i = 0; i < _headers.size(); i++) {
    _chunklist = curl_slist_append(_chunklist, SSTR(_headers[i].first << ": " << _headers[i].second).c_str());
  }

  curl_easy_setopt(handle, CURLOPT_HTTPHEADER, _chunklist);

  //----------------------------------------------------------------------------
  // Start request
  //----------------------------------------------------------------------------
  int still_running = 1;

  while(still_running != 0) {
    if(curl_multi_perform(mhandle, &still_running) != CURLM_OK) {
      break;
    }
  }

  _state = RequestState::kStarted;
  return Status();
}

//------------------------------------------------------------------------------
// Major read function - read a block of max_size bytes (at max) into buffer.
//------------------------------------------------------------------------------
dav_ssize_t StandaloneCurlRequest::readBlock(char* buffer, dav_size_t max_size, Status& st) {
  if(!_session) {
    st = Status(davix_scope_http_request(), StatusCode::AlreadyRunning, "Request has not been started yet");
    return -1;
  }

  if(max_size == 0) {
    return 0;
  }

  st = checkTimeout();
  if(!st.ok()) {
    return -1;
  }

  st = Status();
  return _response_buffer.consume(buffer, max_size);
}

//------------------------------------------------------------------------------
// Finish an already started request.
//------------------------------------------------------------------------------
Status StandaloneCurlRequest::endRequest() {
  _state = RequestState::kFinished;
  return Status();
}

//------------------------------------------------------------------------------
// Check request state
//------------------------------------------------------------------------------
RequestState StandaloneCurlRequest::getState() const {
  return _state;
}

//------------------------------------------------------------------------------
// Check if timeout has passed
//------------------------------------------------------------------------------
Status StandaloneCurlRequest::checkTimeout() {
  if(_deadline.isValid() && _deadline < Chrono::Clock(Chrono::Clock::Monolitic).now()) {
    std::ostringstream ss;
    ss << "timeout of " << _params.getOperationTimeout()->tv_sec << "s";
    return Status(davix_scope_http_request(), StatusCode::OperationTimeout, ss.str());
  }

  return Status();
}

//------------------------------------------------------------------------------
// Get status code - returns 0 if impossible to determine
//------------------------------------------------------------------------------
int StandaloneCurlRequest::getStatusCode() const {
  CURL* handle = _session->getHandle()->handle;
  long response_code = 0;
  curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response_code);
  return response_code;
}

//------------------------------------------------------------------------------
// Do not re-use underlying session
//------------------------------------------------------------------------------
void StandaloneCurlRequest::doNotReuseSession() {

}

//------------------------------------------------------------------------------
// Has the underlying session been used before?
//------------------------------------------------------------------------------
bool StandaloneCurlRequest::isRecycledSession() const {

}

//------------------------------------------------------------------------------
// Obtain redirected location, store into the given Uri
//------------------------------------------------------------------------------
Status StandaloneCurlRequest::obtainRedirectedLocation(Uri &out) {
  if(!_session) {
    return Status(davix_scope_http_request(), StatusCode::InvalidArgument, "Request not active, impossible to obtain redirected location");
  }

  for(auto it = _response_headers.begin(); it != _response_headers.end(); it++) {
    if(strcasecmp("location", it->first.c_str()) == 0) {
      out = Uri(it->second);
      return Status();
    }
  }

  return Status(davix_scope_http_request(), StatusCode::InvalidArgument, "Could not find Location header in answer headers");
}

//------------------------------------------------------------------------------
// Get session error, if available
//------------------------------------------------------------------------------
std::string StandaloneCurlRequest::getSessionError() const {

}

//------------------------------------------------------------------------------
// Block until all response headers have been received
//------------------------------------------------------------------------------
Status StandaloneCurlRequest::readResponseHeaders() {

}

//------------------------------------------------------------------------------
// Feed response header
//------------------------------------------------------------------------------
void StandaloneCurlRequest::feedResponseHeader(const std::string &header) {
  if(header == "\r\n") {
    _received_headers = true;
    return;
  }

  HeaderlineParser parser(header);
  _response_headers.push_back(std::pair<std::string, std::string>(parser.getKey(), parser.getValue()));
}


}