# HaulFromTo / Caravan Route

HaulFromTo / Caravan Route is an open-source native plugin mod for Kenshi, built with RE_Kenshi and KenshiLib.

The mod adds a **C** button beside compatible **Haul To** jobs. Pressing **C** opens a Caravan Route window where you can set a source container, cargo item, optional carrier, and optional guard.

The route leader will collect cargo from the source container, move to the destination container, unload the cargo, and repeat the route automatically.

## Requirements

- Kenshi
- RE_Kenshi
- KenshiLib / RE_Kenshi-compatible plugin setup

## Debug Logging

Debug logging is off by default.

To turn debug logging on, create an empty file named:

```text
HaulFromTo_debug_on.txt

Place it next to HaulFromTo.dll.

To turn debug logging off again, delete or rename that file.


## Step 8 — Add Credits section

## Credits

- **RE_Kenshi** by BFrizzleFoShizzle — provides the native plugin loading and code-injection framework that makes this type of Kenshi plugin possible.
- **KenshiLib** by BFrizzleFoShizzle / KenshiReclaimer — provides reconstructed Kenshi structures and APIs used for accessing variables, calling methods, and hooking game functions.
- **Kenshi** by Lo-Fi Games — original game.
- Thanks to the RE_Kenshi / KenshiLib community and example plugin projects for making Kenshi native plugin development more approachable.
