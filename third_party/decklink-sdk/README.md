# Blackmagic DeckLink SDK headers (16.0)

The `Win/include`, `Mac/include` and `Linux/include` folders of the Blackmagic
Desktop Video / DeckLink SDK 16.0, copied **verbatim** (no edits, original line
endings). Media Player Pro builds its DeckLink input against them; CMake uses
this folder by default, so a clone builds DeckLink support with no SDK download.

## Why they may be here

The SDK's End User License Agreement, clause 0.1, exempts exactly these three
folders from the clauses that limit use, copying and redistribution (1, 4.3,
4.4, 5, 7, 8). Every file in them carries Blackmagic Design's own licence
block, which permits use, reproduction, distribution, sub-licensing and
derivative works provided that block is kept in every source copy. Do not
strip or edit those headers.

Nothing else from the SDK is here, and nothing else may be: the sample code
(`Examples/`, `*/Samples/`) and the documentation stay under the full EULA
(clause 1.7: internal use only). Get them from
<https://www.blackmagicdesign.com/developer/products/capture-and-playback/sdk-and-software>.

Running a DeckLink input still needs Blackmagic's **Desktop Video** driver
installed (at least the SDK's version); it is not part of the SDK and is not
shipped by this project.

"DeckLink" and "Blackmagic Design" are trademarks of Blackmagic Design Pty. Ltd.

## Upgrading

Replace the three `include` folders from the new SDK zip, update the version in
this file's title, and delete any `decklink_gen/` in existing Windows build
trees (midl runs once per tree). Building against a different SDK without
touching this folder: `-DMP_DECKLINK_SDK_DIR=<zip or folder>`.
