# Câblage et pièges rencontrés

## Matériel

- Adoucisseur **JUDO Quicksoft-M** (bornier RS485 **A1/A2** sur la carte de commande).
- **ESP8266 NodeMCU v2** (ou ESP32).
- Module **TTL ↔ RS485 à direction automatique**, alimenté en **3,3 V** (bornes VCC, GND, RX, TX, A, B).

## ESP8266 en mode maître (module Connectivity débranché)

| Module RS485 | NodeMCU |
|---|---|
| RX (données reçues) | **GPIO13 (D7)** — UART0 matériel en position « swap » |
| TX (entrée de l'émetteur) | **GPIO2 (D4)** — UART1 matériel, émission seule (≈1 kΩ en série conseillé) |
| VCC | 3V3 |
| GND | GND |
| A / B | bornier A1 / A2 de la carte JUDO |

Le logger série doit être coupé (`logger: baud_rate: 0`).

## ESP8266 en écoute seule (module Connectivity conservé)

| Module RS485 | NodeMCU |
|---|---|
| RX | GPIO13 (D7) |
| TX | **3,3 V** (le module ne doit jamais émettre) |
| VCC / GND | 3V3 / GND |
| A / B | en parallèle du module JUDO sur A1 / A2 |

## Pièges rencontrés (et solutions)

1. **GPIO3 (RX) inutilisable sur une NodeMCU.** Le convertisseur USB (CP2102 / CH340) y est relié
   par ~470 Ω et maintient la broche à 3,3 V : la sortie du module RS485 ne parvient pas à la tirer
   à 0 (la LED RX du module s'éteint quand on la branche). → utiliser **GPIO13** (UART0 « swap »).

2. **GPIO15 déconseillé pour TX.** GPIO15 est forcément **bas pendant le démarrage** de l'ESP8266.
   Sur les modules RS485 « automatiques » (qui contiennent un petit microcontrôleur), ce long état bas
   **fige le module** : la LED TX clignote, mais plus rien de valable ne passe, jusqu'à une coupure
   d'alimentation du module. → utiliser **GPIO2**, qui est haut au démarrage.
   (Alternative : transistor inverseur sur la ligne TX + `inverted: true`.)

3. **Ne pas inverser TX par logiciel.** Une sortie TX inversée est à l'état bas au repos : le module
   prendrait la ligne en permanence et bloquerait le bus. Si A/B sont croisés, décroiser physiquement.

4. **Un seul maître sur le bus.** Le module Connectivity interroge l'adoucisseur même sans câble réseau.
   En mode maître, il doit être débranché ; l'ESP détecte sa présence et suspend ses requêtes.

5. **Vérifier les connexions du bornier.** Un fil mal serré sur A1/A2 donne exactement les mêmes
   symptômes qu'un problème de protocole (requêtes envoyées, aucune réponse).

6. **Diagnostic.** L'entité « diagnostic bus » affiche `RX … o | trames OK … | CRC err … | TX … | sans réponse …`.
   - `RX 0 o` : rien n'arrive sur la broche RX (câblage, module bloqué).
   - octets reçus mais uniquement des erreurs CRC : polarité A/B ou débit.
   - `TX` augmente, `sans réponse` aussi : l'adoucisseur ne répond pas (bornier, module bloqué, autre maître).
   L'option `log_raw_frames: true` journalise chaque trame et chaque rafale d'octets.
