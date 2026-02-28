# TQVaultC - Titan Quest Vault in C
# Introduction
TQVaultC - Titan Quest Vault in C

# DESCRIPTION
TQVaultC is a Linux-first, Wayland-native, high performance Item and Character manager for the video game Titan Quest Anniversary Edition written in pure C.

It leverages the GTK4 toolkit for GUI elements, Pango / Cairo for text, MagickWand for texture manpiulation, various others.

WHY you ask? ;-) I've been a fan of TQ since it's original release. But I spend more time tinkering with builds and gear than actually playing the game. That means when I 'play' Titan Quest, I'm usually just moving things around in TQVaultAE which runs nicely under wine on Linux. It's very slow however.

I'm an old-school ride-or-die C programmer. tmux + vim has been my "workflow" for decades. However, Agentic AI workflows have become so powerful that I've decided to embrace them. This is my first project.

The first release of TQVaultC is 02/28/2026. I began work on this project on 02/21/2026, spending less than a few hours per evening.

I signed up for Google AI Studio, which comes with one month of free use. Gemini-3-Flash-latest wrote most of the first batch of code however I kept hitting their daily 1M token limit.

I switched to a $20 Claude Code subscription a few days later, Opus 4.6 and I wrote the rest.

I don't have a lot of "recreational coding" time these days, those of you who want support for other platforms are welcome to fork this project. AI Agents can help you port this with minimal effort. I may get around to doing that myself but I don't have any timeline for it.

## DISCLAIMER ##

AI wrote most of this code. Though I use this tool with my live game files and have yet to have any issue or corruption, I recommend that you do not use this tool at all. If you choose to use it, you do so at your own risk as I'm literally telling you not to use it.

## BUILD ##

Just clone the repo and run "ninja -C build && build/tqvaultc". If you get build errors, you might need to install some packages for tools/libraries the program needs. 

Upon invocation, the program will look for it's configuration in $XDG_CONFIG_HOME/.config/tqvaultc -- it will create this folder if it doesn't exist. 

It will also create $HOME/.cache/tqvaultc where it will store some binary blobs that get created after indexing some game assets.

The program needs access to your TQ installation folder for access to game assets and will look by default in the folder $HOME/.local/share/Steam/steamapps/common/Titan Quest Anniversary Edition.

The program also needs access to your game save data folder where it will look for your characters as well as TQVaultAE's vault data. Note that you don't need the latter, but if you have it TQVaultC will support it.

Upon first invocation, TQVaultinC will perform a recursive search for the folder "Titan Quest - Immortal Throne" in $HOME/.local/share/Steam/steamapps/compatdata

The settings dialog will pop up on first invocation giving the user a chance to confirm or edit these paths.

## THANK YOU ##

Big THANK YOU to those who contributed to the TQVaultAE program. Without it, this project would've taken much longer.

## FEATRUES ##

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
