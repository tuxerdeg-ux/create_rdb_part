Das Kommandozeilenprogramm create_rdb_part.c dient der Verwaltung von RDB-basierten Partitionstabellen (Rigid Disk Block) auf Blockgeräten. Es ermöglicht das Erstellen, Ändern, Löschen und Analysieren von Partitionen unter Berücksichtigung spezifischer Zylindergeometrien.
Befehlsübersicht, Syntax & Beispiele
1. Disklabel initialisieren (mklabel)

Schreibt einen neuen RDB-Header in die reservierten Sektoren (0–63) des Datenträgers und setzt die grundlegende Geometrie.

    Syntax: <device> mklabel [rdb] [--force]

    Beispiel:
    Bash

    create_rdb_part /dev/hda mklabel rdb

2. Partition anlegen (mkpart)

Erstellt eine neue Partition mit definiertem Dateisystem, Namen sowie Start- und Endpunkten.

    Syntax: <device> mkpart <name> <fs_type> <start> <end>

    Beispiele:
    Bash

    # Erstellt eine Partition namens 'test' mit SFS und 100 MB Größe in der ersten freien Lücke
    create_rdb_part /dev/hda mkpart test SFS + +100M

    # Erstellt eine Partition namens 'test2' mit SFS2, die den gesamten restlichen freien Platz belegt
    create_rdb_part /dev/hda mkpart test2 SFS2 + +100%

3. Partition löschen (rmpart)

Entfernt eine bestehende Partition anhand ihrer Nummer oder ihres Namens aus der Datenkette und gibt den Block frei.

    Syntax: <device> rmpart <nr|name> [--force]

    Beispiel:
    Bash

    create_rdb_part /dev/hda rmpart 1

4. Partition umbenennen (rename)

Ändert den Namen einer vorhandenen Partition (maximal 31 Zeichen).

    Syntax: <device> rename <nr|old_name> <new_name>

    Beispiel:
    Bash

    create_rdb_part /dev/hda rename DH1 Work

5. Belegung & Freispeicher anzeigen (free)

Gibt eine tabellarische Übersicht der aktuellen Speicheraufteilung, der erkannten Dateisystem-Typen (DosTypes) sowie des unbelegten Freispeichers aus.

    Syntax: <device> free

    Beispiel:
    Bash

    create_rdb_part /dev/hda free

Unterstützte Dateisysteme (<fs_type>)

Das Programm unterstützt diverse Dateisystem-Kennungen und zugehörige DosTypes:

    SFS / SFS2: Smart File System

    OFS / FFS / FFS-INTL / FFS-DC / FFS2: Klassische Dateisystemvarianten

    PFS3: Professional File System

    EXT2 / EXT3: Linux-Dateisysteme

    SWAP: Auslagerungsspeicher

Größen- und Einheitenangaben

Für die Parameter <start> und <end> bei der Partitionserstellung werden folgende Formate unterstützt:

    Absolute Zylinder: Direkte Angabe der Zylindernummer (z. B. 2 oder 500)

    Relative Größen: Angabe mit Maßeinheit wie +500M, +2G oder Byte-Zahlen

    Automatische Lückensuche: + oder auto sucht selbstständig nach dem nächsten passenden freien Bereich

    Prozentuale Zuweisung: 100% nutzt den gesamten verfügbaren Platz in der jeweiligen Lücke
