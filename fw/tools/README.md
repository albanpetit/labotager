# tools/

Outils de développement pour le firmware Labotager.

---

## png2h.py — Conversion PNG → C header RGB565

Convertit des images PNG/JPG en tableaux `uint16_t` PROGMEM compatibles avec TFT_eSPI (`setSwapBytes(false)`), avec masque alpha optionnel.

### macOS / Linux

Setup :

```bash
python3 -m venv tools/.venv
source tools/.venv/bin/activate
pip install -r tools/requirements.txt
```

Utilisation :

```bash
# Background plein écran — opaque, redimensionné à 320×240
python3 tools/png2h.py ma_photo.png \
  --resize 320 240 --opaque \
  --output-dir src/ui/components

# Tous les composants d'un dossier (background sans masque, reste avec masque)
python3 tools/png2h.py tools/images/*.png \
  --opaque-list background \
  --output-dir src/ui/components

# Fichier unique (masque auto-détecté si alpha présent)
python3 tools/png2h.py src/ui/components/home_date.png

# Désactiver le venv
deactivate
```

### Windows (PowerShell)

> Si PowerShell bloque l'activation (`scripts disabled`), exécuter d'abord :
>
> ```powershell
> Set-ExecutionPolicy -Scope CurrentUser RemoteSigned
> ```

Setup :

```powershell
python -m venv tools\.venv
tools\.venv\Scripts\Activate.ps1
pip install -r tools\requirements.txt
```

Utilisation :

```powershell
# Background plein écran — opaque, redimensionné à 320×240
python tools\png2h.py ma_photo.png `
  --resize 320 240 --opaque `
  --output-dir src\ui\components

# Tous les composants d'un dossier
python tools\png2h.py tools\images\*.png `
  --opaque-list background `
  --output-dir src\ui\components

# Fichier unique
python tools\png2h.py src\ui\components\home_date.png

# Désactiver le venv
deactivate
```

### Options

| Option | Description |
|---|---|
| `--opaque` | Force suppression du masque alpha pour tous les inputs |
| `--opaque-list NAME...` | Traite ces noms comme opaques, les autres avec masque |
| `--resize W H` | Redimensionne avec crop-to-fit avant conversion |
| `--output-dir DIR` | Dossier de sortie (défaut : même dossier que l'input) |

### Intégration firmware

Après conversion, remplacer les appels PNG par des appels directs :

```cpp
// Opaque (background)
_tft->pushImage(0, 0, BACKGROUND_W, BACKGROUND_H, (uint16_t *)background);

// Avec alpha (icônes, widgets)
_tft->pushMaskedImage(x, y, ICON_W, ICON_H,
                      (uint16_t *)icon, (uint8_t *)icon_mask);
```
