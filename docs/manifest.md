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

Every downloaded file must then match its `size` and `sha256`. The service's
downloader (`src/core/src/downloader.cpp`) applies this:

- https only, TLS 1.2 or newer, at most 5 redirects and never from https to http (WinHTTP on Windows);
- the list and its signature are capped at 1 MiB and 4 KiB;
- a list whose signature doesn't match is fetched once more after 3 seconds,
  in case CloudFront briefly holds one new file and one old one;
- each file is tried at its URLs in order (the CDN, then the project's own
  server); the body streams to a `.part` file while its SHA-256 is computed,
  and the transfer stops as soon as it goes past the listed size;
- only a file of the exact size and hash is kept. Anything else is deleted
  and the next URL is tried.

The last sequence accepted per key is kept in
`%LOCALAPPDATA%\FloServer\state\manifest-state.json`.

If the list has expired, the app compares the computer's clock with the
server's `Date` header. When they differ by more than an hour it asks the
user to fix the date and time; otherwise the list really is stale and the
message says the problem is on our side. A clock that is behind only
switches off the expiry check; the sequence check still works.

## Installs

`flo install php 8.5` (or the window) asks the service to install the newest
listed version that matches. Runtimes are shared by every project on the
computer, one copy per version (D8, D10):

```
%LOCALAPPDATA%\FloServer\
  runtimes\php\8.5.12\            php-cgi.exe, ext\, the VC++ DLLs...
  runtimes\composer\2.10.3\       composer.phar
  downloads\                       archives while they download
  state\manifest-state.json
```

The archive is unpacked into a hidden `.staging-...` folder next to the final
one. Names that could escape the folder (`..`, absolute or drive paths,
`:`, Windows device names), symbolic links and encrypted entries are
refused before anything is written. A single top folder in the zip
(`node-v24.22.1-win-x64\`) is dropped. The Visual C++ runtime DLLs shipped
in `vcruntime\` next to `floserverd.exe` are copied next to the runtime's
programs (top folder, `bin\`, `Apache24\bin\`) unless the runtime brings its
own. A `.floserver-runtime.json` marker is written last and the folder is
renamed into place, so a folder without a marker is never taken for an
install. Other variants than PHP's usual NTS get their own folder
(`8.5.12-ts`).

## Publishing a new list

The list is built from what the CDN actually serves, after each file has been
compared with the official release. Files live on `https://cdn.floserver.org`
(S3 bucket `floserver-cdn-origin`, eu-central-1, readable only through
CloudFront) at `<runtime>/<version>/<original file name>`.

1. **Upload** new runtime files to the bucket. Keep the original file name, in a
   folder named after the plain version (`php/8.5.12/php-8.5.12-1-nts-Win32-vs17-x64.zip`).
2. **Add them** to the `$files` list at the top of
   `tools/cdn-check/check-floserver-cdn.ps1`, with the project's official
   checksum list (or the original download, for nginx and Apache).
3. **Check** on any computer with internet:
   ```
   powershell -ExecutionPolicy Bypass -File check-floserver-cdn.ps1 -Bucket floserver-cdn-origin -Region eu-central-1
   ```
   Every file must say OK, except Apache: Apache Lounge publishes no
   machine-readable checksum, so it always says CHECK BY HAND. Compare its
   SHA-256 with the checksum on apachelounge.com/download yourself.
   Don't use `-Only` for a run you'll publish from; `build` refuses such a report.
4. **Build** the list from the JSON report, on the same computer:
   ```
   floserver-manifest build floserver-cdn-report.json --sequence <last + 1> --days 30 --accept apache/2.4.69/httpd-2.4.69-261002-Win64-VS18.zip
   ```
   - It writes `manifest.json` in the current folder (`--out <file>` to change that).
   - The CDN address in the list is the one in the report, the address the files were checked on.
   - A file under a known runtime whose name doesn't fit (a typo, a wrong version folder) stops the build.
   - `--accept` names files you compared by hand.
   - Source archives such as MariaDB's `.tar.gz` stay on the CDN for the GPL but aren't listed.
   - The tool knows each runtime's licence, platform and official fallback URL (the catalog in `tools/manifest-tool/build.cpp`).
   - It runs the finished list through the app's own checks before writing it.
5. **Sign** on the offline computer:
   ```
   floserver-manifest sign manifest.json floserver-2026.key
   ```
   It refuses a list whose `expires` is more than 90 days ahead, counted by the
   offline computer's clock. Keep that clock right, or `--days 90` built on
   another computer may be refused.
6. **Check the signature** the way the app will (any computer):
   ```
   floserver-manifest verify manifest.json
   ```
7. **Upload** `manifest.json` and `manifest.json.sig` together to the top of the
   bucket, then invalidate both paths in CloudFront so users get the new pair.
8. **Re-sign before `expires`**, even when nothing changed: build again with the
   next `--sequence`, sign, upload. Otherwise apps stop accepting the list.

Keep a note of the last published `sequence`. Re-using a lower number makes
every app that saw the higher one refuse the list.

## The signing key

- Make it once with `floserver-manifest keygen <key file> floserver-2026`, on an offline machine. The first key, `floserver-2026`, was made on 27 Oct 2026 and is built into `src/core/src/builtin.cpp`.
  - The tool creates the key file readable only by you: an access list on Windows, mode 600 elsewhere. It refuses to overwrite an existing file.
  - The tool prints the public key, which goes into the app's built-in list.
  - The key file never goes on a server, in git, or in chat.
  - Keep a second copy offline, for example on an encrypted USB stick in a safe.
- To change keys, add the new public key to the app first. Sign with the new key only after most users have that app version. The app trusts several keys at once for this.
- If the key leaks, ship an app update that removes it and trusts a new key, then sign with the new key. Its sequence count starts fresh, so whatever the old key signed can't block it.
