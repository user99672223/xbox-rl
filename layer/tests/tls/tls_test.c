/* tls_test.c - gnutls (built against musl by wine/scripts/build-gnutls.sh) doing a real TLS 1.2/1.3 handshake
 * on the layer: DNS, a TCP connection over Winsock, the handshake, certificate verification against the CA
 * bundle, and a small HTTPS request. Wine's schannel is this library behind secur32. */
#include <arpa/inet.h>
#include <gnutls/gnutls.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *host = argc > 1 ? argv[1] : "modules-cdn.eac-prod.on.epicgames.com";
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *ai = NULL;
    int rc = getaddrinfo(host, "443", &hints, &ai);
    if (rc) { printf("tls_test: FAIL getaddrinfo(%s): %s\n", host, gai_strerror(rc)); return 1; }
    int s = socket(ai->ai_family, ai->ai_socktype, 0);
    if (connect(s, ai->ai_addr, ai->ai_addrlen)) { perror("connect"); printf("tls_test: FAIL connect\n"); return 1; }
    printf("tls_test: connected to %s\n", host);

    gnutls_global_init();
    gnutls_certificate_credentials_t cred;
    gnutls_certificate_allocate_credentials(&cred);
    int n = gnutls_certificate_set_x509_trust_file(cred, "/etc/ssl/certs/ca-certificates.crt", GNUTLS_X509_FMT_PEM);
    printf("tls_test: %d trusted CA certificates loaded\n", n);
    gnutls_session_t sess;
    gnutls_init(&sess, GNUTLS_CLIENT);
    gnutls_server_name_set(sess, GNUTLS_NAME_DNS, host, strlen(host));
    gnutls_set_default_priority(sess);
    gnutls_credentials_set(sess, GNUTLS_CRD_CERTIFICATE, cred);
    gnutls_session_set_verify_cert(sess, host, 0);
    gnutls_transport_set_int(sess, s);
    do rc = gnutls_handshake(sess); while (rc < 0 && gnutls_error_is_fatal(rc) == 0);
    if (rc < 0) { printf("tls_test: FAIL handshake: %s\n", gnutls_strerror(rc)); return 1; }
    char *desc = gnutls_session_get_desc(sess);
    printf("tls_test: handshake ok, certificate verified: %s\n", desc);
    gnutls_free(desc);
    const char req[] = "HEAD / HTTP/1.1\r\nHost: modules-cdn.eac-prod.on.epicgames.com\r\nConnection: close\r\n\r\n";
    gnutls_record_send(sess, req, sizeof req - 1);
    char buf[512];
    ssize_t got;
    do got = gnutls_record_recv(sess, buf, sizeof buf - 1); while (got == GNUTLS_E_AGAIN || got == GNUTLS_E_INTERRUPTED);
    if (got > 0) { buf[got] = 0; char *e = strchr(buf, '\r'); if (e) *e = 0; printf("tls_test: server says: %s\n", buf); }
    else printf("tls_test: no response (%zd: %s)\n", got, gnutls_strerror((int)got));
    gnutls_bye(sess, GNUTLS_SHUT_RDWR);
    printf("tls_test: OK\n");
    return 0;
}
