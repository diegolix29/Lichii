# Testing against a local server

Configure > Network > Host redirect sends a game's connections to a server you run, for testing
custom or unlisted NPLN titles. It does nothing while it's off, and it's read when a game starts.

## Settings

- **Host names**: separated by commas or whitespace. Each name covers itself and its
  subdomains, and a leading `*.` means the same, so `*.lp1.t.npln.srv.nintendo.net
  gamesync.npln.nintendo.net` redirects both families. These names resolve to 127.0.0.1.
- **Server address**: `<ip>:<port>`. TCP connections to 127.0.0.1:443 go there instead.
- **CA certificate**: one DER certificate of at most 64 KiB. The guest's TLS trusts it as well
  as the usual roots, for every connection. Issue your server's certificate from it, with the
  redirected names as subject alternative names, and the emulated SSL service accepts it.
- **Block all other network access** (loopback only): names that aren't redirected don't
  resolve, and the guest can reach only this PC and the server address.

## Id tokens

With the redirect and loopback only both on, nn::account answers `LoadIdTokenCache` with a
token Lichii makes for the profile the game asked about. Its `sub` is the profile's id (32 hex
digits), it carries the title version (`tv`), and it has no Nextendo account or nx2 token
(`nnex`). It's signed with a key made for the session, so check its claims and not its
signature. You don't need a Nextendo sign-in, and the title doesn't have to be in Nextendo's game
catalogue. Loopback only means the token can reach only your server.

Without loopback only, games keep their normal Nextendo token.

## Redirect-only mods

Some titles check their servers' certificates or names in their own code, which the CA
certificate doesn't help with. Patch those checks with an ordinary IPS or IPSwitch mod in
`load/<title id>/<mod>/exefs/`, and start the mod's folder name with `[Host redirect]`, for
example `[Host redirect] Local server`. Such mods load only while the redirect is on, and the log
shows a warning for each one it applies. Lichii ships no such patches. You write them for your own
build of the title.
