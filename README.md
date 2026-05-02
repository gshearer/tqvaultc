# TQVaultC - Titan Quest Vault in C
# Introduction
TQVaultC - Titan Quest Vault in C

# DESCRIPTION
TQVaultC is a Linux-first, Wayland-native, high performance Item and Character manager for the video game Titan Quest Anniversary Edition written in pure C. It also works on Windows! :-)

It leverages the GTK4 toolkit for GUI elements and Pango / Cairo for text display.

WHY you ask? ;-) I've been a fan of TQ since it's original release. But I spend more time tinkering with builds and gear than actually playing the game. That means when I 'play' Titan Quest, I'm usually just moving things around in TQVaultAE which runs nicely under wine on Linux. It's very slow however.

I don't have a lot of "recreational coding" time these days, those of you who want support for other platforms are welcome to fork this project. AI Agents can help you port this with minimal effort. I may get around to doing that myself but I don't have any timeline for it.

## DISCLAIMER ##

AI wrote most of this code. Though I use this tool with my live game files and have yet to have any issue or corruption, I recommend that you do not use this tool at all. If you choose to use it, you do so at your own risk as I'm literally telling you not to use it.

## BUILD ##

See BUILD.md

## THANK YOU ##

Big THANK YOU to those who contributed to the TQVaultAE program.

## FEATURES ##

* Ability to manage stacks (potions, scrolls, charms, relics, etc)
* Can apply relics/charms to items using drag/drop
* Change completion bonuses with right-click
* Manage affixes with right-click
* Extreme performance :-)
* Character statistics pane updates as you change items on the character, makes it much easier to put together a build with the resistances and stats you want
* Tries to not allow the user to create items that can't drop naturally in the game, though some cases are very rare such as "of the Tinkerer"

# Main UI
![Main UI](https://raw.githubusercontent.com/gshearer/tqvaultc/refs/heads/main/screenshots/tqvaultc-main.png)

# Affix Manager
![Affix_Manager](https://github.com/gshearer/tqvaultc/blob/main/screenshots/tqvaultc-affix-manager.png?raw=true)

# Build View
![Build_View](https://github.com/gshearer/tqvaultc/blob/main/screenshots/tqvaultc-build-view.png?raw=true)

## TODO ##

* Add the ability to adjust affixes of legendaries according to the Dwarven forge rules
