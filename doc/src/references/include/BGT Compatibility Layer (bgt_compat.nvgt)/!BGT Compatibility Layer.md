# BGT Compatibility Layer
This is an include file that attempts to make old BGT games run in NVGT with minimal modifications, mainly by remapping function names and writing a few convenience wrappers. All function signatures are exactly how they would've been in BGT unless otherwise noted.

Below, you'll find a list of everything this include does, as well as a reference for what each item roughly maps to in native nVGT.

## Key constants:
* KEY_PRIOR: KEY_PAGEUP.
* KEY_NEXT: KEY_PAGEDOWN.
* KEY_LCONTROL: KEY_LCTRL.
* KEY_RCONTROL: KEY_RCTRL.
* KEY_LWIN: KEY_LGUI.
* KEY_RWIN: KEY_RGUI.
* KEY_LMENU: KEY_LALT.
* KEY_RMENU: KEY_RALT.
* KEY_LBRACKET: KEY_LEFTBRACKET.
* KEY_RBRACKET: KEY_RIGHTBRACKET.
* KEY_NUMPADENTER: KEY_NUMPAD_ENTER.
* KEY_DASH: KEY_MINUS.

## Math functions:
* absolute: abs.
* cosine: cos.
* sine: sin.
* tangent: tan.
* arc_cosine: acos.
* arc_sine: asin.
* arc_tangent: atan.
* power: pow.
* square_root: sqrt.
* ceiling: ceil.

## Screen reader speech:
Note that these functions don't map cleanly to how NVGT's API works. As such, substitutions will not be provided here. It is recommended that you use NVGT's built-in screen reader speech functions if you can, they're much more efficient, and much more powerful.

### Constants:
* JAWS.
* WINDOW_EYES.
* SYSTEM_ACCESS.
* NVDA.

### Functions:
* screen_reader_is_running.
* screen_reader_speak.
* screen_reader_speak_interrupt.
* screen_reader_stop_speech.

## String functions:
* string_len: string.length.
* string_replace: string.replace.
* string_left: string.substr.
* string_right: string.slice.
* string_trim_left: string.substr.
* string_trim_right: string.slice.
* string_mid: string.substr.
* string_is_lower_case: string.is_lower.
* string_is_upper_case: string.is_upper.
* string_is_alphabetic: string.is_alphabetic.
* string_is_digits: string.is_digits.
* string_is_alphanumeric: string.is_alphanumeric.
* string_reverse: string.reverse.
* string_to_lower_case: string.lower.
* string_to_upper_case: string.upper.
* string_split: string.split.
* string_contains: string.find.
* get_last_error_text: Superseded by exceptions.
* string_to_number: parse_float.
* string_compress: BGT's own LZ77, not deflate. See below.
* string_decompress: BGT's own LZ77, not inflate. See below.

## String encryption/decryption:
These do work with existing BGT data: they are BGT's own arrangement rather than NVGT's `string_aes_encrypt`. A wrong key is rejected rather than returning rubbish, so `string_decrypt` gives back an empty string.
* string_encrypt: bgt_string_encrypt.
* string_decrypt: bgt_string_decrypt.

## UI functions:
* show_game_window: show_window.
* is_game_window_active: is_window_active.

## Formats BGT wrote, which this layer now reads and writes exactly
A ported game has to read the data its BGT build wrote, and five of these are a *format* rather than a spelling. Each was read out of `bgt.exe` and then checked by asking the engine: the ported function's output is byte for byte what BGT produces, and BGT reads what the port writes.

Every one failed quietly before: the port compiled, ran, and could not read its own old data.

* **generate_computer_id**: `bgt_generate_computer_id`, where this used to answer with `generate_system_fingerprint`. A game checks its registration key against this value, so a different number means a key the player already holds never validates. Agrees with BGT's own answer on 44 of 44 game names, and it is the same algorithm in every engine from BGT 1.3 to the 2019 Manamon build. Windows only, because every component is -- the geometry of PhysicalDrive0, C:'s volume serial, CPUID and the installed memory -- and elsewhere the fingerprint is used instead.
* **string_compress / string_decompress**: `bgt_string_compress` / `bgt_string_decompress`, where this used to use zlib. BGT's is its own LZ77 with a trailing `" <uncompressed length>"`, which is the same codec its module container uses.
* **serialize**: `bgt_serialize`, where this used to call `dictionary::serialize()`. BGT writes one entry as `base64(key)`, a NUL, a type byte, the value as text and a NUL, with 1 an integer, 2 a double written `"%f"` (six decimals, so the precision really is lost), 3 base64 of a string and 4 a bool. An array or a nested dictionary is dropped, which is what BGT does too.
* **deserialize**: unchanged in name, because NVGT registers it -- the engine's own `deserialize` reads BGT's format as well now, so a port loads old saves and new ones alike. NVGT's two formats both begin with a byte that is not base64, so the two can never be confused.
* **file_encrypt / file_decrypt**: `bgt_swcr_encrypt` / `bgt_swcr_decrypt`, where this used `asset_encryptor`. BGT writes `"SWCR"`, the length three times, a tag and AES-256-ECB of the data -- which is also how an entry inside an SFPv asset pack is encrypted. `file_decrypt` returns false on a wrong key rather than writing an empty file.

A BGT save path is `serialize()` into `string_compress()` into `string_encrypt()`, so all three links have to agree: one of them in a different format breaks the whole chain and fails at the decrypt, with nothing to say which layer was wrong.
