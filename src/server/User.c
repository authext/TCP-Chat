#include "User.h"
#include <stdlib.h>
#include <string.h>


User UserCopy(const User* U, va_list v)
{
	(void)v; // silence the unused warning

	User ret = { .Sock = U->Sock };

	strncpy(ret.Name, U->Name, NAME_LENGTH);
	ret.Name[NAME_LENGTH] = '\0';

	return ret;
}

void UserDelete(User *U, va_list v)
{
	(void)U; // silence the unused warning
	(void)v; // silence the unused warning
}


bool SameName(const User* U, va_list args)
{
	return strncmp(U->Name, va_arg(args, char*), NAME_LENGTH) == 0;
}


INSTANTIATE_VECTOR(User, UserCopy, UserDelete);
