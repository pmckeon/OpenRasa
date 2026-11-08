// ClientVersion.h - the version of tabula_rasa.exe OpenRasa reproduces (client 1.16.5.0, built Feb 9 2009).
#pragma once

// Sent in ClientMsg_Login (TRasa::GameClient::LoginToGameServer FUN_004d4500).
#define OPENRASA_CLIENT_VERSION "1.16.5.0"
// The exe's build stamp, as GetClientVersionString (FUN_004d2c20 -> FUN_004d2b50) returns it after the version.
#define OPENRASA_CLIENT_BUILD " (Feb  9 2009 21:11:38) - Shipping"
#define OPENRASA_CLIENT_VERSION_STRING OPENRASA_CLIENT_VERSION OPENRASA_CLIENT_BUILD
