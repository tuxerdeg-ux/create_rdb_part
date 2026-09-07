# create_rdb_part

`create_rdb_part` ist ein kleines, eigenständiges C-Programm zur Verwaltung
von Amiga-RDB-Partitionstabellen unter Linux. Der Schwerpunkt liegt auf
PowerPC-Rescue-Systemen wie dem Pegasos2, das Programm arbeitet aber
grundsätzlich mit Linux-Blockgeräten.

Das Programm bearbeitet die RDB-Struktur direkt auf dem angegebenen Gerät.
Es formatiert keine Dateisysteme und installiert keine Dateisystemtreiber.

> **Warnung:** Die Arbeit mit Blockgeräten und Partitionstabellen kann Daten
> zerstören. Vor jeder Änderung müssen wichtige Daten gesichert werden.

## Funktionen

Unterstützte Operationen:

- Erstellen einer neuen RDB-Partitionstabelle
- Anzeigen vorhandener Partitionen und freier Bereiche
- Anlegen neuer Partitionseinträge
- Löschen von Partitionseinträgen
- Umbenennen von Partitionseinträgen
- automatische Aktualisierung der Linux-Partitionstabelle
- Erstellen und Korrigieren fehlender Linux-Device-Nodes
- Schutz gemounteter Partitionen und aktiver Swap-Partitionen
- deutsche und englische integrierte Hilfe

Dateisystemnamen werden ohne Beachtung der Groß-/Kleinschreibung verarbeitet.
Zusätzlich zu den bekannten Namen können DosTypes als Hexadezimalwert
(`0x...`) oder als vier Zeichen angegeben werden.

## Kommandozeile

```text
create_rdb_part <device> mklabel [rdb] [--force]
create_rdb_part <device> mkpart <name> <fs> <start> <end>
create_rdb_part <device> rmpart <number|name> [--force]
create_rdb_part <device> rename <number|old_name> <new_name>
create_rdb_part <device> free
```

Beispiele:

```sh
create_rdb_part /dev/hda mklabel rdb
create_rdb_part /dev/hda mkpart root EXT3 auto +10G
create_rdb_part /dev/hda mkpart swap SWAP auto +2G
create_rdb_part /dev/hda rmpart 2
create_rdb_part /dev/hda rename root LinuxRoot
create_rdb_part /dev/hda free
```

Hilfe:

```sh
create_rdb_part --help
create_rdb_part --help en
create_rdb_part --help fs
create_rdb_part --help units
```

## Unterstützte DosTypes

| Name | Wert |
| --- | --- |
| `OFS` | `0x444F5300` |
| `FFS` | `0x444F5301` |
| `FFS-INTL` | `0x444F5303` |
| `FFS-DC` | `0x444F5305` |
| `FFS2` | `0x444F5307` |
| `SFS` | `0x53465300` |
| `SFS2` | `0x53465302` |
| `PFS3` | `0x50465303` |
| `SWAP` / `SWP` | `0x53575000` |
| `EXT2` | `0x45585402` |
| `EXT3` | `0x45585403` |

Die DosType-Unterstützung bedeutet ausschließlich, dass der entsprechende
Wert in den RDB-Partitionseintrag geschrieben wird. Die Partition wird
dadurch nicht formatiert.

## Größen- und Bereichsangaben

Für Start- und Endpositionen werden folgende Formen unterstützt:

```text
Zylinder:       20000
Automatisch:    +, auto, next
Relative Größe: +500M, +2G
Prozent:        50%
```

Bei automatischer Platzierung wird die erste geeignete freie Lücke gesucht.
Größenangaben verwenden binäre Einheiten:

```text
K, KB, KiB  = 1024 Bytes
M, MB, MiB  = 1024 * 1024 Bytes
G, GB, GiB  = 1024 * 1024 * 1024 Bytes
```

## Technische Umsetzung

### RDB-Suche und Blockformat

Das Programm arbeitet mit 512-Byte-Sektoren und durchsucht standardmäßig die
Sektoren 0 bis 63 nach einem RDB-Header:

```c
#define SECTOR_SIZE 512
#define RDB_SECTORS_RESERVED 64
#define ID_RDSK 0x5244534B
#define ID_PART 0x50415254
```

Die beiden zentralen Kennungen sind:

- `RDSK` für den RDB-Datenträgerheader
- `PART` für einen RDB-Partitionseintrag

RDB-Werte werden als Big-Endian-32-Bit-Werte gelesen und geschrieben. Dafür
verwendet das Programm eigene Funktionen (`read_be32` und `write_be32`), da
das Zielsystem selbst Little-Endian sein kann.

### Prüfsummen

Nach Änderungen an einem `RDSK`- oder `PART`-Block wird die RDB-Prüfsumme
neu berechnet. Das Prüfsummenfeld wird zunächst auf null gesetzt; anschließend
wird die negative Summe aller 32-Bit-Werte des Blocks eingetragen.

### Partitionskette

RDB-Partitionseinträge sind über das `pe_Next`-Feld verkettet. Beim Anlegen
eines Eintrags wird:

1. ein freier Sektor im reservierten RDB-Bereich gesucht,
2. der neue `PART`-Block geschrieben,
3. die Kette anhand der Zylinderposition eingeordnet,
4. der Vorgänger oder der `RDSK`-Header aktualisiert,
5. die Prüfsumme des geänderten Blocks aktualisiert.

Die Partitionskette wird in Zylinderreihenfolge verwaltet. Dadurch entspricht
die Reihenfolge den Linux-Partitionsnummern besser als eine reine Reihenfolge
der Erstellungszeit.

### Geometrie und freie Bereiche

Die Geometrie wird aus dem RDB-Header gelesen. Verwendet werden insbesondere:

- Sektoren pro Track
- Anzahl der Heads/Surfaces
- erster erlaubter Zylinder
- letzter erlaubter Zylinder

Die Zylindergröße wird berechnet als:

```text
Sektoren pro Zylinder = Surfaces * SectorsPerTrack
Bytes pro Zylinder    = Sektoren pro Zylinder * 512
```

Vor dem Schreiben prüft das Programm:

- dass die Partition innerhalb des Datenträgers liegt,
- dass der reservierte RDB-Bereich nicht überschrieben wird,
- dass sich die Partition nicht mit bestehenden Partitionen überschneidet,
- dass ein freier `PART`-Sektor vorhanden ist.

### Datenträgergröße

Die Größe des Geräts wird bevorzugt über `BLKGETSIZE64` ermittelt. Als
Rückfallmechanismen werden `BLKGETSIZE` und anschließend `lseek` verwendet.
Dadurch kann das Programm auch in eingeschränkten Rescue-Umgebungen mit
älteren Kernel- oder Toolchain-Versionen eingesetzt werden.

### Aktualisierung des Linux-Kernels

Nach Schreiboperationen wird `fsync()` ausgeführt. Anschließend fordert das
Programm mit `BLKRRPART` eine erneute Einlesung der Partitionstabelle durch
den Kernel an.

Wenn das nicht möglich ist, gibt das Programm einen Hinweis auf:

```sh
partprobe <device>
blockdev --rereadpt <device>
```

Device-Nodes werden nur nach einer erfolgreichen Kernel-Aktualisierung
automatisch angelegt oder korrigiert.

## Device-Nodes

Das Programm ermittelt Major- und Minor-Nummer des Basisgeräts mit `stat()`.
Der Minor-Wert der Partition wird aus dem Minor-Wert des Datenträgers plus
Partitionsnummer gebildet. Ein fehlender Node wird anschließend mit `mknod()`
angelegt.

Existiert bereits ein Node, wird geprüft, ob:

- es ein Blockgerät ist,
- Major- und Minor-Nummer zum erwarteten Gerät passen.

Ein veralteter Block-Device-Node kann ersetzt werden. Existiert unter dem
erwarteten Namen dagegen ein Nicht-Blockgerät, wird es aus Sicherheitsgründen
nicht überschrieben.

## Sicherheitsmechanismen

### Schutz aktiver Partitionen

Vor dem Löschen wird `/proc/mounts` und `/proc/swaps` geprüft. Dabei werden
die Geräte über ihre `st_rdev`-Werte verglichen, nicht nur über den Namen.

Gemountete Partitionen und aktive Swap-Partitionen werden nicht gelöscht.

### Bestätigung und `--force`

Das Löschen und Initialisieren einer Partitionstabelle wird standardmäßig
bestätigt. Mit `--force` oder `-f` kann die Bestätigung bei den jeweils
unterstützten Operationen übersprungen werden.

### Fehlerbehandlung

Lesen, Schreiben, Suchen und Aktualisieren werden auf Fehler geprüft.
Schreibvorgänge verwenden `read_exact()` und `write_exact()`, damit auch
partielle POSIX-I/O-Ergebnisse vollständig verarbeitet werden.

Unbekannte Befehle werden mit einem Fehler abgewiesen und nicht als
`mkpart` interpretiert.

## Partitionsnummern

Die logische Partitionsnummer wird aus der Reihenfolge der `PART`-Einträge
ermittelt. Nach dem Löschen einer mittleren Partition können die nachfolgenden
Linux-Partitionsnummern nach unten rutschen.

Das Löschverhalten berücksichtigt diese Neunummerierung: Nach erfolgreichem
Kernel-Re-Read wird nicht blind der entfernte Partitionsname gelöscht,
sondern nur der nicht mehr benötigte höchste alte Node entfernt.

## Bewusste Einschränkungen

Das Programm ist ein RDB-Partitionierungswerkzeug, kein Formatierer und kein
Dateisystemtreiber-Installer. Es kann nicht:

- EXT2, EXT3 oder SWAP formatieren,
- OFS, FFS, SFS, SFS2 oder PFS3 formatieren,
- FSHD-Dateisystemtreiber einbetten,
- AmigaOS- oder MorphOS-Dateisystemtreiber installieren,
- Dateisysteme prüfen oder reparieren,
- Dateien innerhalb einer Partition verwalten.

Nach der Partitionierung müssen passende externe Werkzeuge verwendet werden,
zum Beispiel:

```sh
mkfs.ext3 /dev/hda4
#für Pegasos2 
mkfs.ext3 -F -I 128 -O none,has_journal,sparse_super /dev/hda4
mkswap /dev/hda5
```

Amiga-Dateisysteme müssen auf einem geeigneten AmigaOS-, MorphOS- oder
vergleichbaren System mit den dort verfügbaren Treibern formatiert werden.

## Grenzen der Implementierung

Der reservierte RDB-Bereich umfasst 64 Sektoren. Neue `PART`-Blöcke werden
innerhalb dieses Bereichs gesucht. Die interne Verarbeitung der
Partitionsliste ist auf maximal 64 Einträge begrenzt.

Das Programm ist für klassische Amiga-RDB-Strukturen ausgelegt. Es ersetzt
keine vollständige Partitionsverwaltungsbibliothek und sollte nicht ohne
Prüfung auf unbekannten oder beschädigten RDB-Varianten eingesetzt werden.

## Build

Das Programm benötigt eine Linux-C-Umgebung mit Zugriff auf:

- POSIX-Dateioperationen
- Linux-Blockgeräte
- `ioctl()` und `BLKRRPART`
- `mknod()` für Device-Nodes

Beispiel für einen statischen PowerPC-Build:

```sh
powerpc-linux-gnu-gcc -O2 -static -Wall \
    create_rdb_part.c -o create_rdb_part
```

Für das Pegasos2-Rescue-System wird das Programm in der bestehenden
PowerPC-Buildumgebung gebaut und anschließend in das Sysroot beziehungsweise
die Initrd übernommen.

## Lizenz und Status

`create_rdb_part` enthält keine eingebetteten Amiga-Dateisystemtreiber.
Dadurch werden keine externen SFS-, PFS3- oder FFS-Implementierungen in das
Programm übernommen.

Der aktuelle Funktionsumfang konzentriert sich bewusst auf das Lesen,
Erstellen und Ändern von RDB-Partitionstabellen unter Linux.
