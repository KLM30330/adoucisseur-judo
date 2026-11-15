# Adoucisseur JUDO Quicksoft-M → Home Assistant (ESPHome, RS485)

Intégration locale d'un adoucisseur **JUDO Quicksoft-M** dans **Home Assistant**, sans cloud :
un **ESP8266** (ou ESP32) relié au **bus RS485** de la carte de commande (bornier **A1/A2**)
par un simple **module TTL ↔ RS485** lit les données de l'adoucisseur et les publie via ESPHome.

Deux modes de fonctionnement :

- **Maître** : l'ESP remplace le module JUDO *Connectivity* et interroge lui-même l'adoucisseur.
- **Écoute seule** : le module Connectivity reste en place ; l'ESP décode passivement ses échanges.

> ⚠️ Projet indépendant, **non affilié à JUDO**. Les noms JUDO, Quicksoft et Connectivity sont
> utilisés uniquement pour décrire la compatibilité. Le protocole a été établi par observation du bus.
> Intervenir sur la carte de l'adoucisseur se fait à vos risques (garantie, sécurité électrique :
> **coupez l'alimentation** avant tout branchement).

## Données remontées

| Entité | Source |
|---|---|
| Eau adoucie totale (L) | cmd `0x0029` |
| Eau adoucie aujourd'hui (L) | cmd `0x0018` |
| Débit instantané (L/min) | bloc B `[18-19]` |
| Nombre de régénérations | bloc A `[30-31]` |
| **Régénération en cours** (temps réel) + temps restant de la phase | bloc A `[0]` bit 0, `[14-15]` |
| **Manque de sel** (alerte mémorisée) | bloc A `[0]` bit 7 (électrode de saumure) |
| Capacité restante (%) | bloc A `[6-7]` |
| Dureté de l'eau brute / résiduelle | cmd `0x0052`, bloc A `[26]` |
| Jours avant maintenance + alerte | cmd `0x0026` |
| Jours de fonctionnement | cmd `0x0025` |
| Numéro d'appareil, versions logicielle / périphérique | cmd `0x0006`, `0x0001`, `0x0002` |
| Historique des messages | pages 3/4 de `0x0043` |
| Liaison RS485, dernier événement, diagnostic du bus | — |

La correspondance complète des trames est décrite dans **[docs/PROTOCOLE.md](docs/PROTOCOLE.md)**.

## Matériel

- ESP8266 NodeMCU v2 (testé) ou ESP32
- Module TTL ↔ RS485 à direction automatique, alimenté en 3,3 V
- 4 fils vers le bornier A1/A2 de la carte JUDO

Câblage et pièges rencontrés : **[docs/CABLAGE.md](docs/CABLAGE.md)**.

## Installation (ESPHome)

1. Copier `components/judo_bus/` dans `/config/esphome/components/` (dossier de configuration ESPHome).
2. Partir d'un exemple du dossier [`esphome/`](esphome/) :
   - `judo-esp8266-maitre.yaml` — ESP8266, mode maître (module Connectivity débranché)
   - `judo-esp8266-ecoute.yaml` — ESP8266, écoute seule
   - `judo-esp32-ecoute.yaml` — ESP32, écoute seule
3. Adapter le chemin `external_components` (`path: components` si le YAML est à la racine
   de `/config/esphome/`) et renseigner les secrets (`secrets.yaml.example`).
4. Compiler et installer.

### Options principales du composant `judo_bus`

| Option | Défaut | Rôle |
|---|---|---|
| `master` | `false` | `true` : l'ESP interroge l'adoucisseur |
| `tx_uart_id` | — | UART séparé pour l'émission (ex. UART1 sur GPIO2) |
| `poll_interval` | `500ms` | intervalle minimal entre deux requêtes |
| `capacity_threshold` | `3000` | valeur du compteur de capacité déclenchant une régénération |
| `maintenance_warning_days` | `0` | alerte maintenance N jours avant l'échéance |
| `bus_timeout` | `5min` | délai avant « liaison RS485 perdue » |
| `log_unknown_changes` | `true` | journalise / publie les évolutions de champs non décodés |
| `log_raw_frames` | `false` | journal brut de chaque trame (diagnostic) |
| `power_switch` | — | switch coupant l'alimentation du module RS485 (redémarrage automatique s'il se bloque) |

Un bouton « Adoucisseur sel rechargé » (exemples fournis) acquitte l'alerte sel ; sinon elle
s'efface d'elle-même à la première régénération sans défaut de saumure.

## Contribuer

Les champs marqués ❔ dans `docs/PROTOCOLE.md` restent à identifier. L'entité
« évolution inconnue » publie chaque changement de ces octets : les captures (régénération,
alerte, maintenance, autres modèles JUDO…) sont les bienvenues en *issue*.

## Licence

Code sous licence MIT (voir [LICENSE](LICENSE)).
