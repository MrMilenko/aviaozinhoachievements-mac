#include "Pipe.h"

char pipe_available;

#ifdef _WIN32

HANDLE pipe_handle = INVALID_HANDLE_VALUE;
char pipe_buffer[PIPE_BUFFER_SIZE];
DWORD pipe_bytes_read;
DWORD pipe_bytes_written;

OVERLAPPED g_ov;
HANDLE g_event = NULL;

BOOL Pipe_Create(void) {
    pipe_handle = CreateNamedPipeA(
        PIPE_NAME,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        PIPE_BUFFER_SIZE,
        PIPE_BUFFER_SIZE,
        0,
        NULL
    );
    if (pipe_handle == INVALID_HANDLE_VALUE) return FALSE;
    g_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    ZeroMemory(&g_ov, sizeof(g_ov));
    g_ov.hEvent = g_event;
    return TRUE;
}

DWORD Pipe_AvailableBytes(void)
{
    if (pipe_handle == INVALID_HANDLE_VALUE) {
        return 0;
    }
    DWORD bytesAvailable = 0;
    if (!PeekNamedPipe(pipe_handle, NULL, 0, NULL, &bytesAvailable, NULL)) {
        return 0;
    }
    return bytesAvailable;
}

BOOL Pipe_ConnectToNew(void) {
    return ConnectNamedPipe(pipe_handle, NULL);
}

BOOL Pipe_ConnectToExisting(void)
{
    for (;;)
    {
        pipe_handle = CreateFileA(
            PIPE_NAME,
            GENERIC_READ | GENERIC_WRITE,
            0,
            NULL,
            OPEN_EXISTING,
            0,
            NULL
        );
        if (pipe_handle != INVALID_HANDLE_VALUE)
        {
            DWORD mode = PIPE_READMODE_MESSAGE | PIPE_WAIT;
            SetNamedPipeHandleState(pipe_handle, &mode, NULL, NULL);
            return TRUE;
        }
        DWORD err = GetLastError();
        if (err == ERROR_PIPE_BUSY)
        {
            if (!WaitNamedPipeA(PIPE_NAME, 5000)) 
            {
                return FALSE;
            }
        }
        else if (err == ERROR_FILE_NOT_FOUND)
        {
            return FALSE;
        }
    }
}
BOOL Pipe_Write(const char* format, ...) {
    va_list args;
    va_start(args, format);
    int newByteCount = vsnprintf(pipe_buffer, PIPE_BUFFER_SIZE, format, args);
    va_end(args);
    return WriteFile(pipe_handle, pipe_buffer, newByteCount, &pipe_bytes_written, NULL);
}

BOOL Pipe_Read(void) {
    char result = ReadFile(pipe_handle, pipe_buffer, PIPE_BUFFER_SIZE - 1, &pipe_bytes_read, NULL);
    pipe_buffer[pipe_bytes_read] = '\0';
    return result;
}

void Pipe_Close(void) {
    if (pipe_handle != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(pipe_handle);
        CloseHandle(pipe_handle);
        pipe_handle = INVALID_HANDLE_VALUE;
    }
    if (g_event != NULL) {
        CloseHandle(g_event);
        g_event = NULL;
        g_ov.hEvent = NULL;
    }
}

void Pipe_BeginConnect(void) {
    ResetEvent(g_event);
    BOOL ok = ConnectNamedPipe(pipe_handle, &g_ov);
    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_PIPE_CONNECTED) {
            SetEvent(g_event);
        }
    }
}

BOOL Pipe_IsConnected(void) {
    if (WaitForSingleObject(g_event, 0) == WAIT_OBJECT_0) {
        return TRUE;
    }
    return FALSE;
}

#else /* !_WIN32 */

/* milenko #macport, launcher IPC over an AF_UNIX datagram socket.
 *
 * The Win32 side uses a message-mode named pipe: every Pipe_Write is one message and
 * every Pipe_Read receives exactly one. SOCK_DGRAM gives the same framing, so the
 * protocol above this layer is unchanged. PIPE_NAME is a filesystem socket path here
 * rather than \\.\pipe\..., see the APPLE branch of CMakeLists.txt.
 *
 * The launcher is the server (it binds PIPE_NAME); the engine is the client and binds
 * its own per-pid address so the launcher has somewhere to reply to.
 */

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static int  pipe_fd = -1;
static char pipe_client_path[sizeof(((struct sockaddr_un *)0)->sun_path)];

pipe_handle_t pipe_handle = NULL;
char pipe_buffer[PIPE_BUFFER_SIZE];
pipe_dword_t pipe_bytes_read;
pipe_dword_t pipe_bytes_written;

static void Pipe_SetAddr (struct sockaddr_un *sa, const char *path)
{
	memset (sa, 0, sizeof(*sa));
	sa->sun_family = AF_UNIX;
	strncpy (sa->sun_path, path, sizeof(sa->sun_path) - 1);
}

/* server side, used by the launcher, not the engine */
pipe_bool_t Pipe_Create (void)
{
	struct sockaddr_un sa;

	if (pipe_fd != -1)
		return TRUE;

	pipe_fd = socket (AF_UNIX, SOCK_DGRAM, 0);
	if (pipe_fd < 0)
		return FALSE;

	unlink (PIPE_NAME);
	Pipe_SetAddr (&sa, PIPE_NAME);
	if (bind (pipe_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
	{
		close (pipe_fd);
		pipe_fd = -1;
		return FALSE;
	}

	pipe_handle = &pipe_fd;
	return TRUE;
}

pipe_dword_t Pipe_AvailableBytes (void)
{
	int pending = 0;

	if (pipe_fd == -1)
		return 0;
	if (ioctl (pipe_fd, FIONREAD, &pending) < 0)
		return 0;
	return (pipe_dword_t)pending;
}

pipe_bool_t Pipe_ConnectToNew (void)
{
	return FALSE;	/* no accept() step for datagram sockets */
}

pipe_bool_t Pipe_ConnectToExisting (void)
{
	struct sockaddr_un sa;

	if (pipe_fd != -1)
		return TRUE;

	pipe_fd = socket (AF_UNIX, SOCK_DGRAM, 0);
	if (pipe_fd < 0)
		return FALSE;

	/* our own reply address; per-pid so several engines can share one launcher */
	snprintf (pipe_client_path, sizeof(pipe_client_path), "%s.%ld",
			PIPE_NAME, (long)getpid());
	unlink (pipe_client_path);
	Pipe_SetAddr (&sa, pipe_client_path);
	if (bind (pipe_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
	{
		close (pipe_fd);
		pipe_fd = -1;
		pipe_client_path[0] = '\0';
		return FALSE;
	}

	/* connect() so send()/recv() work and we only hear from the launcher */
	Pipe_SetAddr (&sa, PIPE_NAME);
	if (connect (pipe_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
	{
		close (pipe_fd);
		pipe_fd = -1;
		unlink (pipe_client_path);
		pipe_client_path[0] = '\0';
		return FALSE;
	}

	pipe_handle = &pipe_fd;
	return TRUE;
}

pipe_bool_t Pipe_Write (const char *format, ...)
{
	va_list args;
	int len;
	ssize_t sent;

	if (pipe_fd == -1)
		return FALSE;

	va_start (args, format);
	len = vsnprintf (pipe_buffer, PIPE_BUFFER_SIZE, format, args);
	va_end (args);

	if (len < 0)
		return FALSE;
	if (len > PIPE_BUFFER_SIZE)
		len = PIPE_BUFFER_SIZE;

	sent = send (pipe_fd, pipe_buffer, (size_t)len, 0);
	if (sent < 0)
		return FALSE;

	pipe_bytes_written = (pipe_dword_t)sent;
	return TRUE;
}

pipe_bool_t Pipe_Read (void)
{
	ssize_t got;

	if (pipe_fd == -1)
		return FALSE;

	/* blocking, to match ReadFile on the Win32 path */
	do {
		got = recv (pipe_fd, pipe_buffer, PIPE_BUFFER_SIZE - 1, 0);
	} while (got < 0 && errno == EINTR);

	if (got < 0)
	{
		pipe_bytes_read = 0;
		return FALSE;
	}

	pipe_bytes_read = (pipe_dword_t)got;
	pipe_buffer[got] = '\0';
	return TRUE;
}

void Pipe_Close (void)
{
	if (pipe_fd != -1)
	{
		close (pipe_fd);
		pipe_fd = -1;
	}
	if (pipe_client_path[0])
	{
		unlink (pipe_client_path);
		pipe_client_path[0] = '\0';
	}
	pipe_handle = NULL;
	pipe_available = 0;
}

pipe_bool_t Pipe_IsConnected (void)
{
	return (pipe_fd != -1) ? TRUE : FALSE;
}

void Pipe_BeginConnect (void) {}

#endif /* _WIN32 */
