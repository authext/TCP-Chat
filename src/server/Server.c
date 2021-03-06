#include <arpa/inet.h>
#include <fcntl.h>
#include <regex.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include "Common.h"
#include "Config.h"
#include "User.h"
#include "Conn.h"


// The "main" socket
static int Sock;

VEC(User) Users;
VEC(Conn) Conns;
fd_set master, readfds;

regex_t LoginRegex, TalkRegex, SendRegex;

// Forward declarations
void NewUser();
void Respond(User* U);
void RemoveUser(User *U);


int main()
{
	// Initialize vectors
	Users = VFUN(User, New)();
	Conns = VFUN(Conn, New)();
	printf("Initialized vectors\n");

	// Initialize regexes
	if (regcomp(&LoginRegex, "^" CMD_LOGIN_S REGEX_NAME ";$", REG_EXTENDED) != 0)
		ERROR("regcomp");
	if (regcomp(&TalkRegex, "^" CMD_TALK_S REGEX_NAME ";$", REG_EXTENDED) != 0)
		ERROR("regcomp");
	if (regcomp(&SendRegex, "^" CMD_SEND_S REGEX_TEXT ";$", REG_EXTENDED) != 0)
		ERROR("regcomp");
	printf("Initialized regexes\n");

	// Create the "main" socket, bind it to port and start listening
	if ((Sock = socket(AF_INET, SOCK_STREAM, 0)) == -1)
		ERROR("socket");
	printf("Created the socket\n");

	struct sockaddr_in server =
	{
		.sin_family      = AF_INET,
		.sin_addr.s_addr = INADDR_ANY,
		.sin_port        = htons(PORT)
	};

	if (bind(Sock, (struct sockaddr *)&server, sizeof(server)) == -1)
		ERROR("bind");
	printf("Bound the socket to the port\n");

	if (listen(Sock, 3) == -1)
		ERROR("listen");
	printf("Started listening\n\n");

	// The main processing loop
	int maxfd = Sock;
	FD_ZERO(&readfds);
	FD_ZERO(&master);
	FD_SET(Sock, &master);

	while (true)
	{
		printf("Waiting for connections\n");
		memcpy(&readfds, &master, sizeof(master));

		int ready;
		if ((ready = select(maxfd + 1, &readfds, NULL, NULL, NULL)) == -1)
			ERROR("select");
		printf("Received %d requests\n", ready);

		// Are there new connections?
		if (ready > 0 && FD_ISSET(Sock, &readfds))
		{
			--ready;
			NewUser();
		}

		// Have any existing connections sent some data?
		VEC_FOR(User, it, Users)
		{
			if (ready <= 0)
				break;

			if (FD_ISSET(it->Sock, &readfds))
			{
				--ready;
				Respond(it);
			}
		}

		// Recalculate maxfd, to account for any new users, or for users
		// who have disconnected
		VEC_FOR(User, it, Users)
		{
			if (it->Sock > maxfd)
				maxfd = it->Sock;
		}
	}
}


void NewUser()
{
	int newfd;

	if ((newfd = accept(Sock, NULL, NULL)) == -1)
		ERROR("accept");
	printf("Accepted a new connection to fd %d\n", newfd);

	char inBuffer[BUFFER_SIZE + 1] = "";
	int size = recv(newfd, inBuffer, BUFFER_SIZE, 0);

	if (size == -1)
		ERROR("recv");

	// Client disconnected, nothing to do
	if (size == 0)
		return;

	regmatch_t matches[2];
	char outBuffer[BUFFER_SIZE] = "";

	// Is the command malformed?
	if (regexec(&LoginRegex, inBuffer, 2, matches, 0) != 0)
	{
		snprintf(outBuffer, BUFFER_SIZE, CMD_WTF_S ";");
		printf("The command '%s' did not match the login form\n", inBuffer);
	}
	else
	{
		// Copy the name into a local buffer
		char name[NAME_LENGTH + 1] = "";
		strncpy(name, inBuffer + matches[1].rm_so,
			MIN(NAME_LENGTH, matches[1].rm_eo - matches[1].rm_so));
		printf("New user trying to login with username '%s'\n", name);
		// Check whether the user already exists
		User* it = VFUN(User, Find)(Users, SameName, name);

		if (it != VFUN(User, End)(Users))
		{
			snprintf(outBuffer, BUFFER_SIZE, CMD_NO_S ";");
			printf("A user with the username '%s' already exists\n", name);
		}
		else
		{
			// If the user does not already exists, add to vector
			User newuser = { .Sock = newfd };
			strncpy(newuser.Name, name, NAME_LENGTH);

			VFUN(User, Push)(Users, &newuser);
			FD_SET(newfd, &master);

			snprintf(outBuffer, BUFFER_SIZE, CMD_YES_S ";");
			printf("Successfully added a new user with the username '%s'\n", name);
		}
	}

	send(newfd, outBuffer, BUFFER_SIZE, 0);
	printf("\n");
}


void RemoveUser(User *U)
{
	printf("Removing user with the username '%s' on fd %d\n", U->Name, U->Sock);
	// If the user is a part of a connection, remove it
	VEC_FOR(Conn, cit, Conns)
	{
		if (cit->Sock1 == U->Sock || cit->Sock2 == U->Sock)
		{
			VFUN(Conn, Remove)(Conns, cit);
			break;
		}
	}

	FD_CLR(U->Sock, &master);
	close(U->Sock);
	VFUN(User, Remove)(Users, U);

	printf("Successfully removed\n\n");
}


void Respond(User* U)
{
	char inBuffer[BUFFER_SIZE] = "";
	int ret = recv(U->Sock, inBuffer, BUFFER_SIZE, 0);
	if (ret == -1)
		ERROR("recv");

	if (ret == 0)
	{
		RemoveUser(U);
		return;
	}

	char outBuffer[BUFFER_SIZE + 1] = "";
	char name[NAME_LENGTH + 1]      = "";
	char text[BUFFER_SIZE]          = "";
	User *uit;
	Conn *cit;
	Conn conn;
	regmatch_t matches[2];

	printf("The user with the username '%s' on fd %d sent: '%s'\n",
		U->Name, U->Sock, inBuffer);

	switch (inBuffer[0])
	{
	case CMD_LOGOFF:
		printf("Interpreting the command as logoff\n");
		RemoveUser(U);
		break;

	case CMD_USERS:
		printf("Interpreting the command as users\n");
		// Get the number of logged users and send it
		snprintf(outBuffer, BUFFER_SIZE, CMD_YES_S "%zu;", VFUN(User, Size)(Users));
		send(U->Sock, outBuffer, BUFFER_SIZE, 0);
		printf("Sent number of users: %zu\n", VFUN(User, Size)(Users));

		// Send the usernames one by one
		VEC_FOR(User, it, Users)
		{
			snprintf(outBuffer, BUFFER_SIZE, CMD_YES_S "%s;", it->Name);
			send(U->Sock, outBuffer, BUFFER_SIZE, 0);
			printf("Sent username '%s'\n", it->Name);
		}
		break;

	case CMD_CLOSE:
		printf("Interpreting the command as close\n");
		// If the user is in an active connection, remove it
		cit = VFUN(Conn, Find)(Conns, Active, U->Sock);
		if (cit != VFUN(Conn, End)(Conns))
			VFUN(Conn, Remove)(Conns, cit);
		break;

	case CMD_TALK:
		printf("Trying to match the command as talk\n");
		// Is the command malformed?
		if (regexec(&TalkRegex, inBuffer, 2, matches, 0) != 0)
		{
			snprintf(outBuffer, BUFFER_SIZE, CMD_WTF_S ";");
			send(U->Sock, outBuffer, BUFFER_SIZE, 0);
			printf("The command was malformed\n");
		}
		else
		{
			// Copy the name in a local buffer
			strncpy(name, inBuffer + matches[1].rm_so,
				MIN(NAME_LENGTH, matches[1].rm_eo - matches[1].rm_so));
			printf("The user wants to chat with '%s'\n", name);
			// Check whether the user exists
			uit = VFUN(User, Find)(Users, SameName, name);

			if (uit == VFUN(User, End)(Users))
			{
				snprintf(outBuffer, BUFFER_SIZE, CMD_NO_S ";");
				send(U->Sock, outBuffer, BUFFER_SIZE, 0);
				printf("That user does not exits\n");
			}
			else
			{
				// Check whether the user is in an active connection already
				cit = VFUN(Conn, Find)(Conns, Active, uit->Sock);

				if (cit != VFUN(Conn, End)(Conns))
				{
					snprintf(outBuffer, BUFFER_SIZE, CMD_NO_S ";");
					send(U->Sock, outBuffer, BUFFER_SIZE, 0);
					printf("That user is already talking with someone\n");
				}
				else
				{
					// If the user is not in an active connection, check whether
					// the user is waiting for a response
					cit = VFUN(Conn, Find)(Conns, Waiting, uit->Sock);

					// No, this is a request
					if (cit == VFUN(Conn, End)(Conns))
					{
						conn.Sock1 = U->Sock;
						conn.Sock2 = 0;

						VFUN(Conn, Push)(Conns, &conn);
						snprintf(outBuffer, BUFFER_SIZE, CMD_TALK_S "%s;", U->Name);
						send(uit->Sock, outBuffer, BUFFER_SIZE, 0);
						printf("This is a request\n");
					}
					// Yes, the user is waiting
					else
					{
						if (cit->Sock1 == 0)
							cit->Sock1 = U->Sock;
						else
							cit->Sock2 = U->Sock;

						snprintf(outBuffer, BUFFER_SIZE, CMD_YES_S ";");
						send(uit->Sock, outBuffer, BUFFER_SIZE, 0);
						printf("This is a response\n");
					}
				}
			}
		}
		break;

	case CMD_SEND:
		printf("Trying to match the command as send\n");
		// Is the command malformed?
		if (regexec(&SendRegex, inBuffer, 3, matches, 0) != 0)
		{
			snprintf(outBuffer, BUFFER_SIZE, CMD_WTF_S ";");
			send(U->Sock, outBuffer, BUFFER_SIZE, 0);
			printf("The command was malformed\n");
		}
		else
		{
			// Copy the text into a local buffer
			strncpy(text, inBuffer + matches[1].rm_so,
				MIN(BUFFER_SIZE - 1, matches[1].rm_eo - matches[1].rm_so));

			// Check whether the user is in an active connection
			cit = VFUN(Conn, Find)(Conns, Active, U->Sock);

			if (cit == VFUN(Conn, End)(Conns))
			{
				snprintf(outBuffer, BUFFER_SIZE, CMD_NO_S ";");
				send(U->Sock, outBuffer, BUFFER_SIZE, 0);
				printf("The user is not in an active connection\n");
			}
			else
			{
				snprintf(outBuffer, BUFFER_SIZE, CMD_SEND_S "%s;", text);
				if (cit->Sock1 == U->Sock)
					send(cit->Sock2, outBuffer, BUFFER_SIZE, 0);
				else
					send(cit->Sock1, outBuffer, BUFFER_SIZE, 0);
				printf("Sent the message to the other user\n");
			}
		}
		break;

	default:
		// No idea what was sent
		snprintf(outBuffer, BUFFER_SIZE, CMD_WTF_S ";");
		send(U->Sock, outBuffer, BUFFER_SIZE, 0);
		printf("No idea what the message was supposed to mean\n");
		break;
	}

	printf("\n");
}
