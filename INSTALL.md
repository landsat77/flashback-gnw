# Flashback — installation

Requires Retro-Go SD **v2.0.0-rc1 or later** on the Game & Watch.

The game data is not included: you need the `DATA` folder of the PC DOS
version of Flashback (floppy or CD: `LEVEL1.MAP`, `PERSO.SPR`, `GLOBAL.FIB`...).

1. Build the data pack (Python 3, nothing else to install). Keep the
   `fb_render` folder next to `fb_pack.py`: it renders the music.

       python3 fb_pack.py Flashback.fb /path/to/FLASHBACK/DATA

   On Windows:

       py fb_pack.py Flashback.fb C:\Games\Flashback\DATA

   The pack is about 34 MB. If the external flash of your console is small
   (16 MB), add `--music-codec adpcm` (about 14 MB, a slight hiss in the
   music) or `--no-music` (about 7 MB).

2. Copy to the SD card, side by side, with the same name:

       /homebrews/Flashback.bin
       /homebrews/Flashback.fb

3. Start "Flashback" from the Homebrew tab. The first start copies the pack
   to the external flash: it takes a few minutes.

Controls in a level: D-pad = move (up = jump, down = crouch), A = action
(run, shoot, take, talk), B = draw / put away the gun, TIME = use the
selected object, GAME = inventory, GAME + TIME = game options panel (load /
save game), PAUSE/SET = Retro-Go menu (savestates; Options: music, sound,
language). In menus and cutscenes: A = validate / skip, B = back.

---

# Flashback — installation (français)

Nécessite Retro-Go SD **v2.0.0-rc1 ou plus récent** sur la Game & Watch.

Les données du jeu ne sont pas fournies : il faut le dossier `DATA` de la
version PC DOS de Flashback (disquettes ou CD : `LEVEL1.MAP`, `PERSO.SPR`,
`GLOBAL.FIB`...).

1. Créer le pack de données (Python 3, rien d'autre à installer). Garder le
   dossier `fb_render` à côté de `fb_pack.py` : c'est lui qui calcule la
   musique.

       python3 fb_pack.py Flashback.fb /chemin/vers/FLASHBACK/DATA

   Sous Windows :

       py fb_pack.py Flashback.fb C:\Jeux\Flashback\DATA

   Le pack fait environ 34 Mo. Si la flash externe de la console est petite
   (16 Mo), ajouter `--music-codec adpcm` (environ 14 Mo, léger souffle dans
   la musique) ou `--no-music` (environ 7 Mo).

2. Copier sur la carte SD, côte à côte, avec le même nom :

       /homebrews/Flashback.bin
       /homebrews/Flashback.fb

3. Lancer « Flashback » depuis l'onglet Homebrew. Au premier lancement, le
   pack est copié dans la flash externe : cela prend quelques minutes.

Commandes dans un niveau : croix = déplacements (haut = saut, bas =
s'accroupir), A = action (courir, tirer, prendre, parler), B = sortir /
ranger le pistolet, TIME = utiliser l'objet choisi, GAME = inventaire,
GAME + TIME = panneau d'options du jeu (charger / sauvegarder),
PAUSE/SET = menu Retro-Go (sauvegardes instantanées ; Options : musique,
son, langue). Dans les menus et les cinématiques : A = valider / passer,
B = retour.
