# Pubblicazione su GitHub

## Metodo consigliato da terminale

Apri PowerShell nella cartella del progetto:

```powershell
cd "C:\Users\Loris\Documents\PlatformIO\Projects\Termostato 2 FIX"
```

Inizializza Git:

```powershell
git init
git status
```

Aggiungi i file:

```powershell
git add .
git commit -m "Initial stable thermostat project"
```

Crea il repository su GitHub.

Poi collega il repository locale a quello remoto:

```powershell
git branch -M main
git remote add origin https://github.com/TUO_USERNAME/NOME_REPOSITORY.git
git push -u origin main
```

## Con GitHub CLI

Dalla root del progetto:

```powershell
gh auth login
gh repo create termostato-esp32-s3 --private --source=. --remote=origin --push
```

Per repository pubblico:

```powershell
gh repo create termostato-esp32-s3 --public --source=. --remote=origin --push
```

## Controllo prima del push

Esegui sempre:

```powershell
git status
```

Assicurati che NON compaiano:

```text
.pio/
config.txt
fasce.txt
secrets.h
wifi_credentials.h
.env
```
