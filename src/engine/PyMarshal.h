// PyMarshal.h - Python values from the game server's method calls (tabula_rasa.exe FUN_00951e50 and its "optimized" reader
// TRasa::Get_Optimized_Helper FUN_00953630; the server side is Rasa.NET Rasa.Game/Memory/PythonWriter.cs).
//
// A CallMethod message's argument array starts with a format byte, then one value, then 'f' (0x66) - else the call is dropped
// ("Unable to unpack args for methodName=%s"), as it is when a Python error is left set:
//   'O' (0x4F) the optimized form below (FUN_00953630; the only one Rasa.NET sends);
//   'F' (0x46) a u32 length (< 0x8000) and that many bytes read by the exe's own cut-down marshal reader (FUN_009526d0 ->
//       FUN_0095c270; see MarshalF in PyMarshal.cpp - floats are raw doubles there, unlike Python's marshal);
//   'M' (0x4D) the same raw string read by Python's PyMarshal_ReadObjectFromString (FUN_009525c0).
// Optimized values: the type in the high nibble, a small value or a size form in the low one; multi-byte numbers little-endian.
//   0x00 None, 0x01 True, 0x02 False;
//   0x1n int: n 0..12 the value, 0xD int8, 0xE int16, 0xF int32 (signed - Rasa.NET writes negatives as 0x1F);
//   0x20 long 0, 0x2F int64;
//   0x30 0.0, 0x31 1.0, 0x3E double, 0x3F float;
//   0x40 "" (an empty string, not None), 0x4D / 0x4E / 0x4F length u8 / u16 / u32 (0x8000 and more refused) then the bytes;
//     0x41 a u32 id in the reader's string table - the CallMethod path has none, so it always fails there;
//     0x42 a u32 length and a compression block (kind 0 stored / 1 u32 size + zlib) - the exe accepts it only when FEWER bytes
//     follow than the length says, so in a CallMethod (which must still end in 'f') it always fails; 0x43 - 0x4C fail;
//   0x50 u"", 0x5D / 0x5E / 0x5F length then UTF-8 (strict); 0x52 compressed as 0x42; 0x51, 0x53 - 0x5C fail;
//   0x6n dict of n pairs, 0x7n list, 0x8n tuple: n 0..12 the count, 0xD / 0xE / 0xF u8 / u16 / u32 count;
// nesting deeper than 1000 fails ("maximum recursion level (%d) exceeded").
#pragma once

#include <Python.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace engine {

// The arguments of a call: a new reference, or null with the reason in `error`.
PyObject* unmarshalArguments(const uint8_t* data, size_t length, std::string& error);

// A method call's arguments in the 'O' form, as the exe writes them (TRasa::PythonSerialize::Put FUN_00951c80 -> TRasa::
// Put_Optimized_Helper FUN_00953080): 'O', the value, 'f'. False (nothing written) for a value it cannot write.
bool marshalArguments(PyObject* value, std::vector<uint8_t>& out);

// The three formats against known values and refusals; false and a message on stderr when one is wrong (Python must be running).
bool marshalSelfTest();

}  // namespace engine
