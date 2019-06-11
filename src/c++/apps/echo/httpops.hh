#ifndef HTTPOPS_H_
#define HTTPOPS_H_

#define MAX_FILEPATH_LEN 512
#define MAX_MIME_TYPE 80
#define MAX_REGEX_VALUE_LEN 128

#define FILE_DIR "/media/wiki/"

#define REGEX_KEY "regex="

#define BASE_HTTP_HEADER\
    "HTTP/1.1 %d OK\r\nContent-Type: %s\r\nContent-Length: %d\r\n\r\n"
#define OK_HEADER\
    "HTTP/1.1 200 OK\r\n\r\n"
#define BAD_REQUEST_HEADER\
    "HTTP/1.1 400 Bad Request\r\n\r\n"
#define INT_ERROR_HEADER\
    "HTTP/1.1 501 Internal Server Error\r\n\r\n"
#define NOT_FOUND_HEADER\
    "HTTP/1.1 404 Not Found\r\n\r\n"
#define NOT_IMPLEMENTED_HEADER\
    "HTTP/1.1 501 Not Implemented\r\n\r\n"
#define DEFAULT_MIME_TYPE\
    "text/html"

enum http_req_type {REGEX_REQ, FILE_REQ};

/* HTTP functions */
enum http_req_type get_request_type(char *url);
void replace_special(char *url);
int url_to_path(char *url, const char *dir, char *path, int capacity);
void path_to_mime_type(char *path, char buf[], int capacity);
int generate_header(char **dest, int code, int body_len, char *mime_type);
void generate_response(char **response, char *header, char *body, int header_len, int body_len);

/* Regex functions */
int regex_html(char *to_match, char *htmlDoc, size_t html_length);
int get_regex_value(char *url, char *regex);

#endif