# sco-titanlink-northstar

The Titanfall 2 (Northstar) side of sc-offline's **TitanLink** bridge: a Northstar plugin (a
native DLL) and a small Northstar mod (a Squirrel server script). Together they open the TitanLink
channel that sc-offline creates on the same PC, start a local match when asked, and move the
match's pilot the way Star Citizen tells them to.

It is a separate project from sc-offline and sco-core, MIT licensed, and depends only on
sco-core's two MIT wire headers (`sc_ipc.h`, `sc_titanlink.h`), vendored in `third_party/sco-sdk/`
and pinned to one sco-core version.

Work in progress: the plugin lands through a pull request.
