# AGENTS.md — TitanLink/Northstar bridge charter

Read with README.md and the latest sc-offline and sco-core wire contracts. This is an independent MIT-licensed, local-machine bridge endpoint, NOT the sco-core kernel or its GPL game pack.

- Build against the explicitly MIT-licensed sco-core wire/layout interfaces required by the bridge (especially sc_ipc.h and sc_titanlink.h). Do not introduce GPL host/runtime code into this MIT project without a separately documented license decision.
- Maintain wire versions, binary layouts, validation, shared-memory ownership, timeouts and peer cleanup consistently with sco-core and sc-offline. Changes need cross-repo compatibility tests.
- Keep the bridge local and within published offline safety boundaries. No game/engine binary redistribution, DRM or anti-cheat circumvention, public online access, unauthenticated remote commands or secrets.
- Make scoped reviewable PRs. State exact cross-repo revisions and tests, and distinguish automated integration from tested two-game behavior.
- Respect the sco-core SDK's frozen ABI and independently reviewed three-ZIP release policy. Do not claim the sc-offline megamap crash concerns this project without specific causal evidence.
- Coordinate with current PRs and align external Hermes, cron-bot and OmO instructions to these version-controlled principles.
