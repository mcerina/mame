# Giocare in rete con il rollback

Questa versione di MAME permette di giocare in 2-4 su internet ai giochi co-op, senza il ritardo dei comandi del netplay classico.

> **Stato:** funziona tra due PC Linux, provato con bot e reti simulate. Su Mac non è ancora mai stato compilato né provato. Se qualcosa non va, copia tutto l'output del terminale e mandalo a chi ospita.

Tutti i giocatori devono avere:
- **la stessa versione**, cioè lo stesso commit di questo ramo;
- **le stesse ROM** del gioco.

La partita controlla entrambe le cose, e in caso contrario rifiuta la connessione con il motivo.

## 1. Preparare il Mac (una volta sola)

Apri il Terminale e installa gli strumenti per compilare:

```
xcode-select --install
```

Poi installa [Homebrew](https://brew.sh), se non c'è già, e la libreria SDL2:

```
brew install sdl2
```

## 2. Scaricare e compilare

```
git clone --depth 1 --branch netplay-rollback https://github.com/mcerina/mame.git mame-netplay
cd mame-netplay
netplay/build-macos.sh
```

La prima volta ci vuole da mezz'ora a un'ora, e servono una decina di GB liberi. Alla fine compare `Done: .../np, version ...`.

Il numero dopo `version` deve essere uguale a quello di chi ospita: confrontatelo. Per aggiornare a una versione nuova:

```
git pull
netplay/build-macos.sh
```

Gli aggiornamenti successivi ricompilano solo quello che è cambiato.

## 3. Le ROM

Le ROM non sono incluse. Metti lo zip del gioco, per esempio `ffight.zip`, nella cartella `~/mame-roms`, poi controlla:

```
./np -verifyroms ffight -rompath ~/mame-roms
```

Deve rispondere `romset ffight is good`, oppure `best available`, che per alcuni giochi è il massimo possibile. Alcuni giochi chiedono anche lo zip di un BIOS o di un chip, per esempio `neogeo.zip` o `qsound_hle.zip` per `dino`: se manca, `-verifyroms` dice quale.

## 4. Unirsi a una partita

1. Chi ospita ti manda il suo indirizzo, per esempio `203.0.113.5:7000`.
2. Dalla cartella `mame-netplay`, lancia:
   ```
   netplay/play.sh join ffight 203.0.113.5:7000
   ```
3. Nel terminale compare `Netplay: your address: ...`. Copia l'indirizzo e mandalo in chat a chi ospita. Hai 5 minuti.
4. Quando chi ospita lo incolla, la partita parte su entrambi i computer.

## Ospitare una partita

Su Mac:

```
netplay/play.sh host ffight
```

Su Linux con podman, dalla cartella del progetto: `./netplay.sh host ffight`.

1. Nel terminale compare `Netplay: your address: ...`: mandalo agli altri.
2. Loro ti mandano il proprio indirizzo.
3. Incollalo nel terminale, uno per riga, e premi Invio. Il terminale risponde `Netplay: calling ...`, e quando l'altro entra `player 2 joined`.

Per giocare in 3 o 4: `netplay/play.sh host ffight 7000 3`, e incolla gli indirizzi di tutti.

## Comandi

Ognuno usa i tasti del giocatore 1, anche se nel gioco è il giocatore 2, 3 o 4.

| Azione | Tasto |
|---|---|
| Movimento | frecce |
| Pulsanti 1, 2, 3 | Ctrl, Alt/Option, Spazio |
| Moneta | 5 |
| Start | 1 |

Si possono cambiare dal menu di MAME (Tab), in "Input Settings". Restano salvati in `netplay/cfg`.

Durante la partita pausa, salvataggio, caricamento e reset sono disattivati.

## Se non funziona

**`no answer from the host`.** I pacchetti non passano il router.
- Controlla che chi ospita abbia incollato il tuo indirizzo esatto, mentre la tua finestra era ancora aperta.
- Se uno dei due vede `your router uses a different port for every destination (symmetric NAT)`, il router non permette il collegamento diretto. Succede per esempio con alcuni operatori mobili. Usate Tailscale, qui sotto.

**Tailscale**, quando il collegamento diretto non va:
1. Installatelo entrambi da https://tailscale.com/download.
2. Chi ospita condivide il proprio computer con l'altro, dalla console di Tailscale con "Share", oppure lo invita nella propria rete.
3. Chi si unisce usa l'indirizzo Tailscale di chi ospita, che inizia con `100.`: `netplay/play.sh join ffight 100.x.y.z:7000`.

Con Tailscale non serve incollare niente.

**`refused by the host: different MAME build`.** Le versioni sono diverse, e il messaggio le mostra entrambe. Aggiornate tutti allo stesso commit e ricompilate.

**`refused by the host: different ROM contents`.** Le ROM del gioco non sono identiche. Controllate entrambi con `-verifyroms`.

**`Netplay: repairing a desync`.** I due computer sono andati fuori sincrono. Il gioco si ripara da solo con un piccolo salto. Se succede spesso, manda le righe `Netplay: ... items differ: ...` del terminale.
