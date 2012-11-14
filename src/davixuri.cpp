#include "davixuri.hpp"
#include <ne_uri.h>
#include <cstring>

namespace Davix {

static std::string void_str;

struct UriPrivate{
    UriPrivate(){
        code = StatusCode::UriParsingError;
        memset(&my_uri, 0, sizeof(my_uri));
    }

    UriPrivate(const UriPrivate & orig){
        ne_uri_copy(&my_uri, &(orig.my_uri));
        proto = orig.proto;
        path = orig.path;
        host = orig.host;
        query = orig.query;
        query_and_path = orig.query_and_path;
        code = orig.code;
    }

    ~UriPrivate() {
        ne_uri_free(&my_uri);
    }

    void parsing(const std::string & uri_string){
        if(ne_uri_parse(uri_string.c_str(), &(my_uri)) == 0){

            if(my_uri.scheme == NULL
               || my_uri.path == NULL
               || my_uri.host ==NULL)
                return;

            // fix a neon parser bug when port != number
            if(my_uri.port == 0 && strcasecmp(my_uri.scheme, "http") ==0)
                my_uri.port = 80;

            if(my_uri.port == 0 && strcasecmp(my_uri.scheme, "https") ==0)
                my_uri.port = 443;

            if(my_uri.port == 0)
                return;

            code = StatusCode::OK;
            proto = my_uri.scheme;
            path = my_uri.path;
            host = my_uri.host;
            if(my_uri.query){
                query = my_uri.query;
                query_and_path= path + "?" + query;
            }else{
                query_and_path = path;
            }

        }
    }

    ne_uri my_uri;
    StatusCode::Code code;
    std::string proto;
    std::string path;
    std::string host;
    std::string query;
    std::string query_and_path;

};

Uri::Uri(){
    _init();
}

void Uri::_init(){
    d_ptr = new UriPrivate();

}

Uri::Uri(const std::string & uri)
{
    this->uri_string = uri;
    _init();
    d_ptr->parsing(uri);
}

Uri::Uri(const Uri & uri){
    uri_string = uri.uri_string;
    d_ptr = new UriPrivate(*(uri.d_ptr));
}

Uri::~Uri(){
    ne_uri_free(&(d_ptr->my_uri));
    delete d_ptr;
}

int Uri::getPort() const{
    if(d_ptr->code != StatusCode::OK)
        return -1;
    return d_ptr->my_uri.port;
}

const std::string & Uri::getHost() const{
    if(d_ptr->code != StatusCode::OK)
        return void_str;
    return d_ptr->host;
}

const std::string & Uri::getString() const{
    return uri_string;
}

const std::string & Uri::getProtocol() const {
    if(d_ptr->code != StatusCode::OK)
        return void_str;
    return d_ptr->proto;
}

const std::string & Uri::getPath() const {
    if(d_ptr->code != StatusCode::OK)
        return void_str;
    return d_ptr->path;
}

const std::string & Uri::getPathAndQuery() const {
    if(d_ptr->code != StatusCode::OK)
        return void_str;
    return d_ptr->query_and_path;
}

const std::string & Uri::getQuery() const{
    if(d_ptr->code != StatusCode::OK)
        return void_str;
    return d_ptr->query;
}

Uri & Uri::operator =(const Uri & orig){
    if (d_ptr) delete d_ptr;
    d_ptr = new UriPrivate(*(orig.d_ptr));
    return *this;
}

StatusCode::Code Uri::getStatus() const{
    return d_ptr->code;
}

} // namespace Davix
