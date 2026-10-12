# Updating a device that's already in use — keep all progress

**Short version: update freely. When the installer asks "Erase device?", choose NO.**

## Why that's safe (audited, not assumed)
Progress lives in NVS (profiles, quests, pet, high scores, aquarium, settings).
The installer only writes the bootloader, partition table, app and (if
ticked) the content partition. NVS is never written unless you choose Erase.

- **Partition table is byte-identical** to what deployed devices already have,
  so NVS stays at the same address, untouched.
- **Nothing stores progress in the content partition.** The only file the
  firmware ever writes there is the one-shot to-do import deleting itself.
  Reinstalling content (guides, packs, aquarium art) is safe.
- **No saved-data format changed** in any app that already existed:
  quest profile and pet state are byte-identical to the deployed code; the
  aquarium's two new fish were appended after the existing species, the decor
  table is untouched, and Mantis Fishing's live catches use a new namespace
  ("aqlive") rather than growing the saved profile.
- **New apps use new namespaces** (mvaders, mchat, mpsych, ...) and start fresh.
- **Removed apps** (Storms/Fires) left nothing behind that anything reads.
- **App numbers are not saved anywhere**, so adding/removing apps can't
  scramble saved data.

## When to choose Erase = YES
Only for a brand-new board, or to deliberately reset a device to factory.
