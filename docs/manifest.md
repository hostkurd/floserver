# The runtime manifest (D5)

FloServer downloads PHP, MariaDB, Node.js, Composer, nginx and Apache from a
list on our server. The list is signed, so a hacked mirror or a hijacked
connection can't send users a different file.

## Files

Both files are published together, on the main server and every mirror:

- `manifest.json`: the format is in `src/core/include/floserver/core/manifest.h`.
- `manifest.json.sig`: `{"key": "<key id>", "signature": "<base64>"}`.

The app checks the Ed25519 signature over the exact bytes of `manifest.json`
before reading anything in it. After that it refuses the list when:

- it has passed its `expires` time, so a stale copy can't hold users on an old version;
- its `sequence` is lower than the last one the app accepted from the same key, so an older list can't be replayed;
- its `sequence` jumps more than 100,000 ahead of that, so a typo or a stolen key can't push the number out of reach.

The app counts sequence numbers per signing key, so a new key starts its own count.

Every downloaded file must then match its `size` and `sha256`. The downloader that applies this comes with the service.

If every mirror's list reads as out of date, the user's clock may be ahead. The app should then ask them to check the date and time. A clock that is behind only switches off the expiry check; the sequence check still works.

## Publishing a new list

1. Edit `manifest.json`. Raise `sequence` by one. Set `expires` about 30 days ahead.
2. Run `floserver-manifest sign manifest.json <key file>` on the signing machine. The tool checks the list opens before writing the `.sig`.
3. Upload both files: mirrors first, then the main server.
4. Re-sign before `expires`, even when nothing changed: raise `sequence` and move `expires` on. Otherwise apps stop accepting the list.

## The signing key

- Make it once with `floserver-manifest keygen <key file> hostkurd-2026`, on an offline machine.
  - The tool creates the key file readable only by you: an access list on Windows, mode 600 elsewhere. It refuses to overwrite an existing file.
  - The tool prints the public key, which goes into the app's built-in list.
  - The key file never goes on a server, in git, or in chat.
  - Keep a second copy offline, for example on an encrypted USB stick in a safe.
- To change keys, add the new public key to the app first. Sign with the new key only after most users have that app version. The app trusts several keys at once for this.
- If the key leaks, ship an app update that removes it and trusts a new key, then sign with the new key. Its sequence count starts fresh, so whatever the old key signed can't block it.
