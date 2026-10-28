/*-------------------------------------------------------------------------
 * otel_http.c
 *	  Minimal HTTP/1.1 POST for OTLP/HTTP export.  Plain http:// only: the
 *	  intended target is a collector on the same host or pod.  One short-lived
 *	  connection per export keeps this free of state; at one export per flush
 *	  interval the cost is irrelevant.
 *
 * Runs in the background worker only.  All socket operations are bounded by
 * the caller's timeout.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "otel.h"

static int
connect_with_timeout(const char *host, const char *port, int timeout_ms,
					 char *err, int errlen)
{
	struct addrinfo hints,
			   *res,
			   *ai;
	int			rc,
				fd = -1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	rc = getaddrinfo(host, port, &hints, &res);
	if (rc != 0)
	{
		snprintf(err, errlen, "cannot resolve %s: %s", host, gai_strerror(rc));
		return -1;
	}

	for (ai = res; ai != NULL; ai = ai->ai_next)
	{
		struct pollfd pfd;
		int			soerr = 0;
		socklen_t	sl = sizeof(soerr);

		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		(void) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

		rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
		if (rc == 0)
			break;
		if (errno == EINPROGRESS)
		{
			pfd.fd = fd;
			pfd.events = POLLOUT;
			if (poll(&pfd, 1, timeout_ms) == 1 &&
				getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 &&
				soerr == 0)
				break;
			errno = soerr ? soerr : ETIMEDOUT;
		}
		snprintf(err, errlen, "cannot connect to %s:%s: %m", host, port);
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);

	if (fd >= 0)
	{
		struct timeval tv;

		(void) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
		tv.tv_sec = timeout_ms / 1000;
		tv.tv_usec = (timeout_ms % 1000) * 1000;
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}
	else if (err[0] == '\0')
		snprintf(err, errlen, "cannot connect to %s:%s", host, port);
	return fd;
}

bool
otel_http_post(const char *url, const char *body, int len, int timeout_ms,
			   char *err, int errlen)
{
	char		host[256];
	char		port[16] = "80";
	char		path[512] = "/";
	const char *p;
	const char *hend;
	char		hdr[1024];
	char		resp[256];
	int			hlen,
				fd,
				n,
				got = 0,
				status = 0;

	err[0] = '\0';

	if (strncmp(url, "http://", 7) != 0)
	{
		snprintf(err, errlen, "unsupported endpoint '%s': only http:// is supported; use a local collector for TLS", url);
		return false;
	}
	p = url + 7;

	if (*p == '[')				/* [v6]:port */
	{
		const char *close_br = strchr(p, ']');

		if (!close_br || close_br - p - 1 >= (int) sizeof(host))
			goto bad_url;
		memcpy(host, p + 1, close_br - p - 1);
		host[close_br - p - 1] = '\0';
		hend = close_br + 1;
	}
	else
	{
		hend = p + strcspn(p, ":/");
		if (hend - p == 0 || hend - p >= (int) sizeof(host))
			goto bad_url;
		memcpy(host, p, hend - p);
		host[hend - p] = '\0';
	}
	if (*hend == ':')
	{
		int			plen = (int) strcspn(hend + 1, "/");

		if (plen == 0 || plen >= (int) sizeof(port))
			goto bad_url;
		memcpy(port, hend + 1, plen);
		port[plen] = '\0';
		hend += 1 + plen;
	}
	if (*hend == '/')
		strlcpy(path, hend, sizeof(path));
	else if (*hend != '\0')
		goto bad_url;

	fd = connect_with_timeout(host, port, timeout_ms, err, errlen);
	if (fd < 0)
		return false;

	hlen = snprintf(hdr, sizeof(hdr),
					"POST %s HTTP/1.1\r\n"
					"Host: %s:%s\r\n"
					"User-Agent: " OTEL_SCOPE_NAME "/" OTEL_VERSION "\r\n"
					"Content-Type: application/x-protobuf\r\n"
					"Content-Length: %d\r\n"
					"Connection: close\r\n\r\n",
					path, host, port, len);

	/* header, then body */
	for (n = 0; n < hlen;)
	{
		ssize_t		w = send(fd, hdr + n, hlen - n, MSG_NOSIGNAL);

		if (w <= 0)
			goto io_error;
		n += (int) w;
	}
	for (n = 0; n < len;)
	{
		ssize_t		w = send(fd, body + n, len - n, MSG_NOSIGNAL);

		if (w <= 0)
			goto io_error;
		n += (int) w;
	}

	/* Only the status line matters. */
	while (got < (int) sizeof(resp) - 1)
	{
		ssize_t		r = recv(fd, resp + got, sizeof(resp) - 1 - got, 0);

		if (r <= 0)
			break;
		got += (int) r;
		if (memchr(resp, '\n', got))
			break;
	}
	resp[got] = '\0';
	close(fd);

	if (sscanf(resp, "HTTP/%*d.%*d %d", &status) != 1)
	{
		snprintf(err, errlen, "no valid HTTP response from %s", url);
		return false;
	}
	if (status < 200 || status >= 300)
	{
		snprintf(err, errlen, "collector responded HTTP %d", status);
		return false;
	}
	return true;

io_error:
	snprintf(err, errlen, "I/O error talking to %s: %m", url);
	close(fd);
	return false;

bad_url:
	snprintf(err, errlen, "invalid endpoint URL '%s'", url);
	return false;
}
