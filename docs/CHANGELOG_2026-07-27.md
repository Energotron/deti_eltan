fa96c7d  fix: keep the new-game detour off the Universe engine

Opening the build guards let this one install there for the first time,
and party generation then stalled at "time passing" -- a step that has
never given trouble before. The detour runs the new-game thread's Execute
a second time to build the first-arm sidecar, and on that engine
something in it does not come back.

It is stock-only until that is understood. Nothing is lost by it: the
sidecar it produces is written again by the portal when it opens, and a
game that will not start a new party is worse than a mod that cannot yet
jump.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
8dfbd32  fix: two more guards that knew only the Steam build

Same shape as the timestamp: constants written when there was one engine
to know. SizeOfImage is 0x4d1000 there and 0x4d2000 on Universe, and the
check refused anything else, so every path guarded by it declined before
reaching an address at all.

TGalaxy's class cell went in too. It sits 0xd8c ahead of the constructor,
the same distance that constructor moved, and resolving the live galaxy
compares the object's first word against it -- without it that comparison
was reading the Steam build's layout and failing every time.

Both are neighbour-derived rather than found, like the thread pair before
them. The signature checks still stand behind them.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
e4e33a8  fix: the patch guard only ever knew one engine

Two of the engine patches refuse to install unless the executable's PE
timestamp matches a constant. That check is right and it stays -- writing
into a binary you have not identified is how you corrupt someone's game.
But it was written when there was one build to know, and it kept saying
no on Universe long after every address it needed had been mapped and
long after the anchor had proved the adapter itself loads and runs there.

It now knows both stamps: 0x68eccc46 for the Steam build, 0x6a4e23a9 for
Universe. Anything else is still refused.

This is what hook-install-failed was, both before and after the class
reference was mapped -- the address table was never the thing standing in
the way.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
b138848  fix: the new-game hook could not install on the Universe build

Good news first: the anchor answered at all. "The anchor cannot find the
other half of the corridor" is this mod's own message, raised by its own
OnUseCode after asking its own DLL -- so the adapter loads, injects and
runs on the Universe engine. That was the part in doubt.

What it was refusing on: dual_newgame_status 0 and hook-install-failed.
The patch that hooks the new-game thread verifies a signature at the
thread class reference, and that address was never mapped, so it was
still pointing into the Steam build's layout and the check declined --
correctly.

The class reference sits immediately ahead of Execute, which is mapped,
and moves with it. The TThread constructor sits between two helpers that
both moved by 0xd80. Both are read off their neighbours rather than found
outright, which is weaker than the rest of the table -- but a wrong guess
here costs nothing, because the signature check is exactly what stops it.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
76b618e  fix: put the module back where the manager looks for it

The manager said it plainly once the info panel was opened: "location:
ChildrenOfEltan". It resolves the name from ModCFG straight to
Mods\ChildrenOfEltan and reads ModuleInfo there -- it does not descend.
Ours was a directory deeper, so it found no author, no description and no
files, and filed us under "not found" while thirty Universe modules sat
happily in their own sections.

The nesting only ever existed to carry ShuKlissan inside this mod. The
Universe pack ships it now, so there is nothing left to carry and the
module goes back to being one folder. Every path that moved down a level
this morning moves back: the cache entries, the adapter path in
ScriptLibs, and the path the launcher injects from.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
6d5d11e  fix: the mod manager would not take the module as it was packaged

It listed us under "not found" with no description and no files, next to
thirty Universe modules it was perfectly happy with. Those modules are
CFG, DATA and ModuleInfo, nothing else. Ours also carried a package and
an INSTALL.TXT pointing at it, which is how this mod has shipped its
script since before any of the Universe pack was involved.

The package is gone and the script travels loose, the way theirs do. That
means addressing it by its real path from the game root -- data\... only
resolves for a file inside a package, which is the same lesson the galaxy
backdrop taught this morning from the other direction.

The launcher stays in the folder. It is not part of what the manager
reads, and the game has to be started through it for the adapter to be
injected at all.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
562b268  feat: the adapter knows which engine it is patching

Every address in this file now goes through one lookup instead of being
compiled in. The stock values stay where they were; a table beside them
carries what each becomes on the Universe build, and the lookup picks
between them once, at first use.

Telling the builds apart costs nothing: .text is 4667148 bytes in the
Steam executable and 4671200 in Universe, read straight out of the PE
header already mapped into the process. A renamed file cannot fool it.

An address with no entry falls through to its stock value on purpose.
Every patch that writes to the engine checks a byte signature at its
target first and refuses when it does not match, so an address that was
never mapped declines to install rather than corrupting whatever happens
to live there. That is what the hook-install-failed lines in this
morning's log were -- the checks doing their job on an engine they had
never seen.

Twenty-six addresses mapped: every one the transit needs, including the
two derived by hand. What is left unmapped belongs to the abandoned
pointer-swap machinery and to crash guards that were written for it.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
1cce7f3  docs: both addresses the transit cannot work without, derived

The automatic passes could not place either, so they were done by hand.

The load-path cell came out of a shift the data section makes as a whole:
.data starts one page later in the Universe build, and every cell moves
with it. Checked rather than assumed -- the count of instructions
referencing each cell is identical on both sides, 7 and 7, 30 and 30,
3559 and 3559. A wrong address does not reproduce that.

The save-preview routine was recompiled and its bytes match nothing, so it
came through its callers instead: find the call site, read the target from
the relative operand.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
c5e981b  refactor: drop the per-turn diagnostics

Eleven imports and their call sites, running every single turn since the
days when nobody knew where the galaxy pointer lived: raw pointer probes,
layout observations, cluster surveys, star/sector count comparisons, ship
watches, save-format version checks, the AB-test log and the LoadGame
diagnostics. They answered their questions months ago and have been
writing sixteen megabytes of JSONL per session ever since.

The heartbeat and the checkpoints stay -- those are what actually gets
read when something goes wrong, and they are two lines a turn.

This also settles six of the seventeen addresses still unmapped onto the
Universe build: five belonged to the LoadGame diagnostics and one to a
form gate that was found long ago to patch the function selecting
background music, and was never removed.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
e197a36  feat: 37 of 54 engine addresses mapped onto the Universe build

Three passes, each picking up what the one before could not.

Byte windows find code, but only once relocated dwords are masked out --
they are absolute addresses, they differ between builds by definition,
and the interesting code is exactly the code that mentions globals.
Masking them took this from 20 to 35.

Global cells have no bytes to search for, so they go through the code
that touches them: locate the instruction, read its operand back out.
That is where the form cells, the galaxy import cell and the save manager
came from.

Import slots come from the import directory by name.

What is left is seventeen call sites and immediates sitting inside
functions whose starts are known but whose insides were laid out
differently by the newer compiler. Searching within the enclosing
function only helps when an anchor is near enough, and for these it is
not. They need reading, one at a time.

The table so far is in docs/UNIVERSE_ADDRESS_MAP.txt.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
9e8aa3d  feat: map the engine addresses onto a different build of Rangers.exe

The Universe community ships its own executable, and it is a separate
compilation rather than a patch -- 78% of .text differs and everything
after the first kilobyte moves. All fifty-odd addresses the adapter
patches have to be found again, and once per engine build by hand is not
a plan.

Two kinds of address, two methods. Code is found by its own bytes: whole
functions survive recompilation unchanged more often than not, so a
window taken at the known address usually appears exactly once in the new
build; where it ties, the window widens, and where it fails it is
reported rather than guessed. Global cells have no bytes to search for,
so they go through the code that touches them -- find an instruction
whose operand is the cell, locate that instruction in the new build, read
the operand back. Both move; what they are to each other does not.

Encouraging sign from the earlier survey: every byte signature the
adapter already carries still exists in the Universe build, none missing.
The engine is the same program, recompiled.

Needs both executables side by side, and the stock one is currently
nowhere on this machine.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
9c54326  feat: the Klissans ship inside this mod now

One folder to install. Mods\ChildrenOfEltan is a pack: this mod, plus
ShuKlissan and the UtilityFunctionsPack it depends on, each still the
build its own author shipped. Only the paths inside their CFG were
rewritten, since the engine addresses a mod's files from the game root
and moving a folder otherwise orphans every one of them -- two hundred
in ShuKlissan's case.

Everything this mod owns moved down a level with it: the cache entries,
the adapter path in ScriptLibs, the path the launcher injects from, and
the package line in INSTALL.TXT.

Worth recording, because it cost a startup: UtilityFunctionsPack lived
under Mods\Tweaks, ShuKlissan declares it a hard dependency, and deleting
Tweaks took the game down with a crash on the splash screen and nothing
in any log to say why. It travels in the pack now, so removing anything
else cannot repeat that.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
9ee2722  feat: tooling to embed another author's module into this one's pack

Space Rangers addresses a mod's loose files by their path from the game
root, and a module writes those paths into its own CacheData -- two
hundred of them in ShuKlissan's case. Move the folder and every one
points at nothing, which the engine answers by drawing an element with no
picture in it and taking the galaxy map down, exactly as happened here
earlier. So embedding is not a copy: the .dat files are decompiled,
their prefix rewritten, and built again.

Only CFG/*.dat are touched, and only the path prefix in them. Artwork,
scripts and ModuleInfo cross over byte for byte, which keeps the embedded
module the author's build rather than our re-reading of it.

Verified on both modules the Klissans need: ShuKlissan itself, 105 MB,
and UtilityFunctionsPack, which it declares as a dependency.

Also drops the script size check added earlier today. ShuKlissan ships a
669678-byte script that the same engine loads without complaint, so the
64 KB ceiling I inferred from our own failure does not exist and the
guard was encoding a wrong belief. What actually broke that launch is
still unexplained.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
4b325d7  fix: the script had grown past what the engine will load

"Failed to launch, do you want to try restarting without mods?" and
nothing else -- no crash, no exception, the process exiting cleanly with
zero. The mod's files were all well formed and the adapter exported
everything the config declared. What changed was size: the compiled
script went from 57050 bytes, which loaded, to 67378, which does not.
65536 sits between them, and the engine says nothing about it.

The .scr keeps its source as UTF-16, so every character of comment costs
two bytes. Eighty of those lines were commented-out dead code from the
Phase-0 galaxy-pointer experiment, plus a paragraph about why the arcade
loading cover was abandoned -- both long settled, both already in the
history, neither of them anything the engine should have been carrying.
Removing them puts it back to 57090.

The build now refuses to produce a script that large, since the symptom
gives no hint where to look.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
1114dfe  feat: a couple of Klissan nests take hold in the second arm

Two systems, three creatures each, seeded once. The nest flag lives in
the save, so finding one already flying their colours is what stops this
repeating, and the cap keeps the swarm where the spec wants it: wild
nests, not an empire.

Only when ShuKlissan is installed. It owns the hulls, the guns, the names
and the artwork; handing a ship one of its types with none of its Lang
behind it leaves the engine looking up strings that do not exist. Asking
for one of those strings is how this tells whether the mod is there, so
without it the arm quietly goes without Klissans instead of breaking.

Deliberately not a hard Dependence line. The engine enforces that by
module name, ShuKlissan ships no ModuleInfo in the copy I was given, and
guessing wrong would stop this mod loading at all. The description says
what it adds instead.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
ac7bf8e  feat: the sweep spares the Klissans, and keeps them few

ShuKlissan builds its creatures out of Dominators and repaints them --
BuyDomikExtremal, then ShipType 'Klissan1'..'Klissan5' and a 'Klissan'
custom faction on top. So every Klissan reads as t_Kling, and yesterday's
sweep would have wiped the swarms out along with the thing they are meant
to replace. The custom type tells them apart and, unlike the faction,
needs no script group membership to read.

Their systems are spared too: a system flying StarCustomFaction 'Klissan'
is a nest, and nests are not handed back to anyone.

The spec asks for very few of them -- wild swarms and nests, not an
empire -- so past a handful in one system the rest is culled regardless
of what difficulty ShuKlissan was set to.

Worth writing down while it is fresh: the spec does allow Blazer, Keller
and Terron to reach the second arm later, as tracked invasions with their
own fronts. The blanket sweep would refuse them forever. That machinery
does not exist yet, so this stands, but it is a wall the invasion work
will have to knock down rather than build around.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
8a47122  fix: an ordinary galaxy was being written over the authored Eltan map

All three of today's failures were one thing. CE_Eltan_SecondHome.sav no
longer held Second Home: reading it back showed Taron, Phedok, Solntse
and planets called Mercury, Venus and Earth. So the sky stayed hidden and
the Dominator sweep never ran -- both hang off a check that asks whether
this is the Eltan map, and honestly it was not -- and the planets showed
whatever races the ordinary galaxy had.

Opening the portal saves the world you are leaving, and it picked the
file by g_ce_active_arm. That is a DLL global: it does not survive
quitting the game. Once it disagreed with reality, a jump out of the
first arm wrote an ordinary galaxy over the prepared map. The map is
authored content and nothing in the game can rebuild it, so this was the
one file that must never be guessed at.

It now asks the galaxy instead. Star zero of the Eltan map carries a name
that map alone gives it, systems live at galaxy+0x2c and TStar keeps its
name at +0x10, so the answer is read straight out of the world and the
flag is set from it rather than trusted. The script asks the same
function, so the two cannot disagree again.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
251a082  feat: the pirate clan does not reach the second arm either

Same sweep, one more faction. A system either side holds goes back to the
people who live on it, each planet gets a defender rather than being left
naked, and the ships are marked.

One deliberate narrowing: a pirate is only taken if the clan owns him.
ShipTypeN cannot tell a clan pirate from a freelancer -- both read
t_Pirate -- but ShipOwner can, and freelancers are scenery rather than a
faction. An arm with no pirates at all would be a very quiet place. Say
the word and the check comes out.

Ordinary pirate bases are left standing for the same reason: they are
stations the arm trades with. BlockPirates only ever dropped the
Dominion, and this follows it.

Clan aggression joins the two Dominator settings at its floor.

PirateWin(3), which BlockPirates calls to close the HD pirate storyline,
is deliberately not called: it ends a story rather than removing a
faction, and what happens to that arm's own story is not this sweep's
business.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
a32d515  feat: no Dominators in the second arm

The master spec says they are not there, so they are removed the way
BlockPirates removes the pirate clan -- that mod turned out to be the
readable answer to a question the script reference does not cover.

Same shape, one faction over: hand any system they hold back to the
people who live on it, give each planet a defender so it is not left
naked, drop the Dominion, and mark their ships. ShipDestroy leaves no
wreckage to loot, so nothing here is farmable. Their ships wander, so
every system is swept rather than only the ones they own.

Spawn frequency and aggression go to their floor as well. The engine
offers no zero -- fifty is as low as the setting reads -- so the sweep is
what actually holds the line; the setting only means it has less to do.

All of it sits behind the same Second Home check as the sky, so the first
arm keeps its war.

Also backs out the fog attempt. Rescale on HideBuf changed nothing.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
7965f36  feat: it is called Children of Eltan now, and its own list entry is legible

The mod list showed its Russian lines as mojibake because ModuleInfo is
read as UTF-16LE -- a working mod from another author starts with a byte
order mark and CRLF, and ours went out as UTF-8. It stays plain UTF-8 in
the repository, where it is readable and diffable, and is converted on
the way out.

Dropped "Smoke" from the name and from the folder the mod lives in. It
stopped being a smoke test a long time ago. The description now says what
it actually does rather than promising to change nothing.

Also a first attempt at the fog over unopened sectors. That picture is
fetched by the engine straight from the cache and appears nowhere in
Main.dat, so a CacheData entry would replace it in both arms. The buffer
it is drawn into does have a name, and GraphBuf takes an image through
Rescale, so it is asked for there instead -- only while in Second Home.
Untested against the engine.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
ab1aabe  fix: stop the map refresh from cancelling the engine's own form change

The pending-form byte is not ours to take. On the return leg the log
caught it holding 20 -- the engine had already queued a form and had not
reached it yet -- and the refresh overwrote that with the star map. The
queued form never happened, so the map came up while the game still had
the player docked, which read as being dumped on a station.

It now writes only into an empty byte. A transition the engine has
already queued redraws the screen on its own, so there was never anything
to add in that case. The log says whether the write happened.

The backdrop goes back to z 999. In front of HideBuf at 9 it covered the
map itself rather than just the fog over closed sectors.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
d9c6e8e  fix: the image registry was in the wrong place and pointed nowhere

Fairan's galaxy map mod is the same job done correctly, and it shows two
things this was getting wrong. Image entries live under a Bm section, not
at the top of CacheData, so ours registered nothing at all. And a loose
file in a mod folder is named by its full path from the game root --
Mods\...\DATA\... -- not by the virtual data\ path, which only resolves
for what ships inside a pkg. Our script does ship in the pkg, which is
why that one entry worked and hid the mistake.

So the backdrop element was pointing at a name that was never registered,
at a file that could not be found, and the galaxy map died the moment it
tried to draw it. That, not the form merge, is what has been crashing.
The element goes back in, unchanged.

The anchor icon had both faults too and has never once been shown.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
16e682c  feat: the map catches up on arrival instead of on the next day

Nothing was actually late. The log has the transfer applied immediately
after the load, before any day passes; what lagged was the picture. The
engine runs the arrival code while it is still inside the call that opens
the star map, so the map had already been drawn from the numbers the
arriving save carried.

FormChange('StarMap') cannot fix that, and reading the engine says why:
it looks up the target form, sees it is the one already showing, and
takes a shorter path that sets a flag rather than queueing an entry.

The main loop wants one byte. It reads a pending form index out of a
known cell and, for the star map, calls that form's enter method and
zeroes the byte itself. Writing the index directly buys exactly one extra
rebuild, after the traveller's money, date, skills and gear are in place,
and leaves no state behind. The same call ends the return leg.

It logs what the two form cells held at the time, so if the rebuild still
does not happen there is something to read rather than guess at.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
f0e10cc  fix: pull the galaxy backdrop back out; launcher borrows the game icon

The backdrop kept killing the galaxy map, both as a bare Image among the
stock siblings and wrapped in a named container, so it is out until the
merge rules are actually known rather than inferred. The image stays in
the module and keeps its CacheData entry, which costs nothing and is not
what was crashing; only the form element and the script override are
gone. The first arm was never touched either way.

The launcher shows the game icon now. It is a separate process, so
Windows drew it with the generic application icon while the thing it
starts had its own. The icon is lifted out of the installed Rangers.exe
at build time rather than committed here -- it is the game artwork, and
this repository has no business shipping a copy. No game path, no icon,
and the build carries on regardless.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
f8db232  fix: map opened onto an element with no picture in it

The backdrop was merged in with an empty Image block standing in for the
stock one, on the assumption that BlockPar matches unnamed siblings by
position. If it appends instead, that placeholder is not a placeholder at
all -- it is a real Image element carrying no picture, no size and no
position, and the galaxy map died the moment it tried to draw it. The
transfer itself came through intact; the log has a checkpoint for every
item and runs clean to the end of arrival.

It now hangs off a named container block, the way the anchor cheat panel
already does, which cannot merge into a stock sibling under either
reading. The script only asserts the override while the game is actually
in Second Home, so nothing is touched at all in the first arm.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
6bcea20  fix: crash on arrival, from feeding CreateHull an item type

CreateHull takes the hull class -- 0 ranger, 1 military, 2 pirate -- as
its first argument, not the item type constant. Every hull reports
ItemType 42, so 42 is what the transfer handed it, and the engine indexed
its hull table with 42 and read off the end. Access violation, process
gone, arrival log stopping at the checkpoint just before the restore
loop. It only started firing now because the hull was never captured
before this week: the old capture loop began at item 1 and item 0 is
always the hull.

The hull is no longer rebuilt at all. It is edited where it sits --
HullType, ItemSize, ItemLevel, ItemOwner, then Chameleon for the skin --
which says the same thing and never leaves the ship without one. The
stash carries the hull class as a ninth field, since the item type alone
cannot express it.

The restore loop no longer guesses at types it cannot build. Custom
artefacts and custom weapons are named things, not numbered ones, and
CreateArt/CreateEquipment do not make them; nodes and quest items have no
numeric constructor either. Those are skipped rather than fed to a
constructor that would misread them. Loose micromodules and probes now
come across properly, through CreateMM and CreateZond.

Every item logs a checkpoint carrying its type on the way in and on the
way out, so the next failure of this kind names the item instead of
leaving the whole loop under suspicion.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
72f52e4  feat: the Eltan sky hangs over Second Home only

The background file has been shipping in the module since it was drawn,
but nothing ever pointed the engine at it. What did exist pointed at the
wrong thing twice over: it registered under FormGalaxy.2bg, a cache entry
no form actually reads (the galaxy map draws FormGalaxy2.2bg), and had it
been correct it would have replaced the sky in both arms, since a
CacheData key shadows the base game everywhere.

So it registers under its own key instead, and rides on its own Image
element added to the Galaxy form, which stays hidden until the game says
we are in Second Home. The answer comes from the world, not from a flag:
star 0 of the Eltan map carries a name no vanilla galaxy has, so it still
holds after quitting and reloading, which a DLL global would not.

The element is merged in positionally, the way SR2LoadingScreen does it
-- empty sibling blocks stand in for the panels being skipped -- and sits
at z 999, one step in front of the stock backdrop and behind everything
else on the map.

Also refuses to build on a comment holding an unpaired apostrophe, which
is what cost an afternoon here: RScript reads it as an unterminated
string and reports the error somewhere else entirely.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
6d04ddd  fix: the traveller's kit arrives fitted, not duplicated in the hold

Four things went wrong on arrival and they had one root between them:
the transfer knew what each item was but nothing about where it sat.

  * The capture loop started at item 1, so the hull -- always item 0 --
    was never taken. Everything else was rebuilt on top of the Second
    Home captain's own factory kit, which is why the traveller arrived
    with two of everything, their own half loose in the hold.
  * Nothing recorded the slot, so even a clean set would have landed in
    the hold. ItemIsInUse() reads the slot on the way out and installs
    into it on the way back.
  * Artefacts live in their own list on the ship and were skipped
    entirely; so were goods, which are not items at all.
  * Wear, akrin and micromodule went unrecorded, so a battered upgraded
    weapon came back factory-fresh. Stored with 1 added so that 0 can
    mean "not recorded" -- a worn-out item legitimately reads 0.

Arrival no longer picks a star by distance and TransferShip()s there.
That was a leftover from when the transition swapped galaxy pointers
under a live game and the ship had to be dragged along by hand; now that
arriving is a load, the ship is already somewhere valid and moving it
only tore the camera loose. The exit hole is placed next to the ship
instead -- the mirror of what the Anchor does on departure.

ShipCalcParam() and UpdateFormShip() rebuild the ship and its own form
but not the star map, which is why money and date only showed up after a
day was skipped. FormChange('StarMap') re-enters the form, which is the
documented way to make it read the world again.

The stash file grew items alongside the scalars and its magic changed,
so a file written by the previous build is rejected rather than read
with the fields shifted.

Also: RScript treats ' as a string delimiter inside comments too, so a
comment containing "traveller's" swallowed the code after it and the
compiler reported the error hundreds of characters away. The comments
here avoid apostrophes.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
ef29f74  feat: put ships behind the Eltan greeting

The arrival message announces the Council's order to take the ranger alive,
which reads hollow with an empty sky around the ship. BuyWarrior spawns a
military vessel at a planet, so three now appear at the arrival system before
the message is delivered.

Guarded on the arrival star existing and having planets, since the escort is
flavour and must never be the thing that breaks a transit.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
915013f  feat: carry the hold and fittings across, and refresh the ship form on arrival

Equipment turned out to need no string plumbing at all. ItemType, ItemSize,
ItemLevel and ItemOwner describe an item entirely in numbers, and
CreateEquipment / CreateHull / CreateArt rebuild one from exactly those, so
the existing integer machinery covers cargo and fittings as they stand. An
earlier conclusion in this project -- that items could not be carried because
their names could not be returned to RScript -- was wrong twice over: names
can be returned, and they are not needed.

The artifact walks ShipItems and stashes four numbers per item; the arrival
block recreates each one, branching to CreateHull for a hull and CreateArt for
an artefact and falling through to CreateEquipment otherwise, then adds it to
the ship.

Also fixes the reported lag in what the player sees: transferred values only
appeared after skipping a day. ShipCalcParam makes the ship recount itself
after being re-fitted, which its own documentation recommends after any
artificial refit, and UpdateFormShip redraws the ship form immediately.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
ce64244  fix: the arrival fired before the load, so everything landed in the old world

Checkpoints told the story: 110, 135, 136 all logged, meaning the captain
transfer really did run and really did set money, skills and experience -- and
none of it showed up in play.

It ran in the wrong galaxy. Arming and polling sit in the same Turn
invocation: CompleteRegisteredPortal near the top of the turn code, the
pending-arrival poll a few lines below. A zero-tick countdown therefore fired
immediately, while the departing arm was still live and FormChange('GameLoad')
had not taken effect. The money went to the player about to be discarded and
the arrival hole was opened in the galaxy about to be replaced.

Zero came from the previous fix, which correctly identified that a 2-tick
countdown never elapsed (the traveller lands docked, where turns do not pass)
but overcorrected. One tick is the right number: it lets the load complete,
and Turn-code runs once immediately afterwards.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
2cd1267  fix: arrival died on the docked traveller's sector, taking the transfer with it

Checkpoint trail placed through the arrival block ended at 140 and never
reached 141, which puts the failure on a single call: SectorVisible with the
sector taken from StarToCon(ShipStar(Player())).

The traveller is docked on arrival -- TransferShip rebinds the system but does
not undock -- and ShipStar on a docked ship does not yield one, so a garbage
sector reached SectorVisible and aborted the rest of the block. Everything
after it was collateral: money, skills, experience and the date all live
further down and simply never ran, which is exactly what the player reported.

Two changes:

- the sector now comes from the arrival star, which is known good because the
  hole was already created against it, and both it and the neighbour sweep are
  guarded on it being valid;

- the captain transfer moves to the very top of the block, immediately after
  the entry checkpoint. The traveller's money and experience should not depend
  on map cosmetics succeeding first.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
6ef1d02  feat: keep authored Second Home, carry the captain both ways

Three changes, plus repairs to damage an earlier scripted edit did.

- Second Home is now authored content. Once the sidecar exists on disk,
  generation skips it entirely rather than overwriting: the file is built from
  a normally-saved game and rewritten by tools/make_second_home.py. This also
  retires the second Execute, which was the source of both the wrong captain
  (it builds an entire new game, its own player included) and the half-built
  interface those mid-generation saves carried into play. Regenerating means
  deleting the file.

- The captain transfer now runs on the return leg too, not just on the way
  out. Capture always happened in whichever arm the artifact was used in, so
  it was already symmetric; only the arrival half was one-sided. Duplicated
  with distinct variable names because RScript rejects the same name declared
  in two branches.

- Repaired asm and format-string literals that a previous scripted rewrite had
  split across real newlines, breaking the build. Verified the diff touches
  nothing else.

Honest limit worth recording: while the traveller is in one arm the other is
frozen on disk exactly as it was left. The date is carried forward, but the
world behind it does not simulate the days that passed, so "the other arm
lives while you are away" is not delivered by this architecture.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
362466b  feat: carry the date across the corridor

A loaded save brings its own date, so a traveller leaving on turn 500 landed
on whatever turn the target world happened to be saved at -- which for a
corridor between two halves of the same galaxy reads as time travel.

TGalaxy.turn sits at +0x4C (ranger-tools' game-objects/TGalaxy.h names it
outright). The artifact now stashes CurTurn() alongside the captain's numbers
and the arrival block writes it into the freshly loaded galaxy.

Only ever moves the date forward. Rewinding would make days the target world
has already processed happen a second time, so a target already ahead of the
traveller is left alone rather than dragged back.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------
c8bf0fb  tools: build the Second Home arm out of an ordinary save

Rewrites everything the player reads off the map into Children of Eltan
naming: 73 systems, 330 planets and 31 stations, all unique.

Systems lead with the places the story points at -- Эльтанская Рана, Сердце
Пепла, Врата Второго Дома, Город Незажжённых, Игла Айока -- then continue
through the forge and ash registers. Planets follow vanilla's actual shape,
single invented words built from Eltan roots, rather than the "adjective plus
noun" phrasing that reads nothing like the original table. Stations are keyed
by engine type, a split read off the base save: 6 military, 7 pirate,
8 weapons, 9 science, 10 business, 11 medical.

Left alone on purpose, and the reasons matter:

- races and owners. The Eltan peoples reuse the engine's five slots (Maloc
  reads as Strongs, Peleng as Agills, People as Mediums, Fei as Intells, Gaal
  as the Fifth Treaty menzols), so this is a reskin rather than a data change
  and reassigning owners would only scramble the galaxy's politics.
- NPC ship names, over a thousand of them, generated per race and therefore
  carried by the same reskin.
- graph_name / graph_object_type / face, which are the visual pass.

The base must be a normally-saved game. Saves written mid-generation carry a
half-built interface and draw a hangar dialog over the star map -- verified on
an untouched sidecar, so it is not an artefact of editing.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>

----------------------------------------