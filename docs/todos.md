# Écran carré et version enfants — reste à faire

Branche `feat/square-s3` (fusionnée avec `feat/kids-p4`). Builds concernés :
`esp32s3_square` (GUITION ESP32-4848S040, 480×480) et `esp32_4inch_kids`
(P4 portrait 480×800), qui partagent l'interface de `src/square/`.
Maquettes : canevas « Sonos carré 480 », pages « Carré 4″ (S3) » et
« Portrait 4″ (P4) ».

État au 15 novembre 2026 : rien n'est poussé, pas de PR.

## Appli

Fait le 15 novembre 2026. Compilé pour les trois builds et essayé sur le
panneau carré (veille, réveil avalé, pochettes relues depuis la flash,
vérification OTA sur `Beungoud/SonosESP`). **Pas encore essayé** : une vraie
installation OTA (le fork n'a pas encore de release), le premier démarrage
sur un panneau vierge, et tout le build enfants sur le P4 :

- [x] **Mise en veille** (`sq_sleep.cpp`, écran Veille dans Réglages) : baisse
      après 30 s / 1 min / 5 min, mode nuit avec heures et écran éteint ou
      veilleuse. Mêmes clés NVS que le P4 classique. Le toucher qui réveille un
      écran presque noir n'appuie sur rien.
- [x] **Écran de premier démarrage** (`sq_setup.cpp`) : après le Wi-Fi,
      enceintes, comptes Spotify, favoris, puis pochettes gardées en flash.
      Une seule fois (clé NVS `sq_setup_done`).
- [x] **Mises à jour par Wi-Fi (OTA)** (`sq_ota.cpp`, écran Mise à jour) :
      releases GitHub de `GITHUB_REPO`, canaux stable et d'essai. L'installation
      redémarre le panneau et flashe au démarrage, avant tout le reste. Assets
      attendus : `firmware-square.bin`, `firmware-4inch-kids.bin`,
      `firmware-4inch-r3-kids.bin` (à ajouter aux workflows de release).
- [x] **Vignettes en flash** (`sq_art.cpp`) : les pochettes des favoris sont
      gardées décodées dans LittleFS (`/art/<clé>.565`) et supprimées quand le
      favori disparaît.

## Défauts connus

- [ ] Ouvrir une liste de 30 éléments donne un à-coup d'environ 0,6 s
      (construction des lignes sur le fil de l'interface).
- [ ] Les JPEG progressifs de plus de 640 px (pochettes Radio France,
      1000 px) ne sont pas décodés : visuel de remplacement.
- [ ] Le « œ » (« cœur ») s'affiche comme un carré vide dans les paroles sur
      le P4 portrait : vérifier les plages des polices utilisées.
- [ ] Maquettes : les boutons de volume font encore des pas de 10 (le
      firmware fait des pas de 1).

## À reporter sur le firmware P4 classique

Corrigé dans le code de l'écran carré, mais pas dans celui du P4 classique
(`esp32_4inch*`) :

- [ ] **Tactile** : le GT911 renvoie parfois « aucun doigt » entre deux
      mesures. Un appui est alors coupé en deux (doubles clics, clic à la fin
      d'un défilement). Correctif carré : un relâchement n'est compté
      qu'après ~100 ms sans doigt (`touch_driver.cpp`, branche `UI_SQUARE`).
- [ ] **Décodeur PNG** : PNGdec garde deux lignes dans
      `PNG_MAX_BUFFERED_PIXELS` mais n'en vérifie qu'une. Un logo radio large
      déborde sur le tas. Correctif carré : tampon agrandi et images trop
      larges refusées (`sq_art.cpp`, `platformio.ini`).

## Projet

- [ ] Pousser la branche et ouvrir une PR.
- [ ] Ajouter `esp32s3_square` et `esp32_4inch_kids` à l'intégration
      continue, à l'installateur web et aux versions publiées.
- [ ] Alléger les journaux de diagnostic avant une version publique :
      `[SLOW]`, `[STALL]`, `[SQ/JOB]`, `[SQ/FS]` (toujours actifs) ;
      `[PERF]` et les commandes série de banc ne sont compilés qu'avec
      `UI_DEBUG_TAP`.

## Hors de portée (pour mémoire)

- Artistes suivis Spotify : non exposés par l'intégration Sonos (seule l'API
  Web Spotify les donne).
- Recherche dans la bibliothèque musicale locale : non faite.
