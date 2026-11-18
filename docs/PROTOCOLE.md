# Protocole du bus RS485 JUDO (Quicksoft-M)

Ce document décrit le protocole observé entre le module **JUDO Connectivity** (réf. 1702929)
et la carte de commande d'un adoucisseur **JUDO Quicksoft-M**. Il a été établi par
**observation passive du bus** (écoute des échanges), puis vérifié en interrogeant
l'adoucisseur depuis un ESP8266. Aucun firmware JUDO n'a été lu, copié ni décompilé.

> Les valeurs marquées ✅ sont confirmées (comparées à l'application JUDO ou au compteur d'eau).
> Les valeurs marquées 🔶 sont des hypothèses fortes, ❔ restent inconnues.
> Testé sur : Quicksoft-M, version logicielle 1.05, version périphérique 5.20, module Connectivity 1.030.

---

## 1. Couche physique

| Paramètre | Valeur |
|---|---|
| Support | RS485 2 fils, bornier **A1/A2** de la carte de commande |
| Vitesse | **19 200 bauds** |
| Format | **8N1** (8 bits, sans parité, 1 bit de stop) |
| Topologie | maître / esclave : le **module Connectivity est maître**, la carte de l'adoucisseur ne parle **jamais spontanément** |
| Temps de réponse de la carte | 35 à 70 ms |

Si A et B sont croisés, le signal reçu est simplement inversé (en écoute, `inverted: true` sur la broche RX suffit ;
en émission il faut décroiser physiquement).

---

## 2. Format d'une trame

Requêtes et réponses ont le même format :

```
┌──────┬──────┬───────────┬───────────┬────────────┬───────────┐
│ ADDR │ TYPE │  CMD (LE) │  LEN (LE) │  DATA[LEN] │  CRC (LE) │
│ 1 o  │ 1 o  │   2 o     │   2 o     │   LEN o    │   2 o     │
└──────┴──────┴───────────┴───────────┴────────────┴───────────┘
```

| Champ | Description |
|---|---|
| `ADDR` | adresse, toujours `0x01` |
| `TYPE` | type d'appareil : **`0x38` = Quicksoft-M** (le module balaie `0x32`…`0x39` au démarrage) |
| `CMD` | numéro de commande, 16 bits petit-boutiste |
| `LEN` | longueur des données, 16 bits petit-boutiste |
| `DATA` | données (absentes dans une requête de lecture) |
| `CRC` | **CRC-16/CCITT-FALSE** (poly `0x1021`, init `0xFFFF`, sans réflexion, sans XOR final), calculé sur **tous les octets précédents**, transmis **octet de poids faible en premier** |

- Une **requête de lecture** a `LEN = 0` (sauf la commande `0x0043`, qui porte 1 octet : le numéro de page).
- La **réponse** reprend `ADDR`, `TYPE` et `CMD`, avec les données.

### Exemple

```
Requête  : 01 38 02 00 00 00 1B A8            → lire la commande 0x0002
Réponse  : 01 38 02 00 02 00 14 05 F7 45      → 2 octets : 14 05 = version 5.20
```

### CRC (C)

```c
uint16_t crc16_ccitt_false(const uint8_t *d, size_t n) {
  uint16_t crc = 0xFFFF;
  while (n--) {
    crc ^= (uint16_t)(*d++) << 8;
    for (int i = 0; i < 8; i++)
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
  }
  return crc;   // émis : crc & 0xFF, puis crc >> 8
}
```

---

## 3. Séquence de démarrage du module Connectivity

Capturée à la mise sous tension du module :

```
01 32 02 00 00 00 B5 EE   ┐
01 33 02 00 00 00 E4 44   │  balayage des types d'appareils 0x32…0x39,
01 34 02 00 00 00 30 23   │  une requête toutes les ~2 s,
01 35 02 00 00 00 61 89   │  jusqu'à obtenir une réponse
01 36 02 00 00 00 B3 67   ┘
01 38 02 00 00 00 1B A8   → réponse 14 05        (version périphérique 5.20)
01 38 01 00 00 00 C7 33   → réponse 64 05 01     (version logicielle 1.05)
```

Il n'y a **pas d'ouverture de session** : un autre maître (l'ESP) peut interroger directement la carte.
Sans réponse, le module recommence le balayage toutes les ~13 min.

---

## 4. Commandes simples

| CMD | Long. réponse | Contenu | Statut |
|---|---|---|---|
| `0x0001` | 3 | version logicielle : `[0]` ❔ (0x64), `[1]` mineur, `[2]` majeur → `64 05 01` = **1.05** | ✅ |
| `0x0002` | 2 | version périphérique / matérielle : `[0]` mineur, `[1]` majeur → `14 05` = **5.20** | ✅ |
| `0x0006` | 4 | **numéro d'appareil**, u32 LE | ✅ |
| `0x0018` | 4 | `[2-3]` **eau adoucie depuis minuit**, u16 LE, en litres ; `[0-1]` = 0 | ✅ |
| `0x0021` | 1 | `02` (langue ?) | ❔ |
| `0x0023` | 1 | **unité de dureté** : `2` = °f | 🔶 |
| `0x0025` | 4 | **temps de fonctionnement** : `[0]` minutes, `[1]` heures, `[2-3]` jours u16 | ✅ |
| `0x0026` | 6 | `[0-1]` **heures avant maintenance**, u16 LE (÷24 = jours) ; `[2-5]` = `00 00 18 00` ❔ | ✅ |
| `0x0028` | 4 | eau brute (toujours 0 sur ce modèle) | 🔶 |
| `0x0029` | 4 | **eau adoucie totale**, u32 LE, en litres | ✅ |
| `0x0033` | 1 | `0x21` | ❔ |
| `0x0052` | 2 | **dureté de l'eau brute**, u16 LE (30 = 30 °f) | ✅ |
| `0x0054` | 2 | dureté de l'eau brute (copie de 0x0052) | ✅ |
| `0x0055` | 5 | zéros (stock de sel, non géré par ce modèle ?) | ❔ |
| `0x0056` | 4 | zéros | ❔ |
| `0x0017` | 0 | requête horaire du module, sans données | ❔ |
| `0x000D` / `0x000E` | 4 | **écritures** du module vers la carte, octets variables d'un démarrage à l'autre (horodatage ?) — inutiles pour lire les données | ❔ |

---

## 5. Commande `0x0043` : blocs d'état paginés

La requête porte 1 octet de page : `01 38 43 00 01 00 <page> CRC CRC`.

| Page | Réponse | Contenu |
|---|---|---|
| 0 | 32 o | **bloc B** |
| 1 | 32 o | **bloc A** (état principal) |
| 2 | 32 o | **bloc C** |
| 3 | 64 o | historique des messages : jours |
| 4 | 64 o | historique des messages : codes |

### 5.1 Bloc A (page 1) : état principal

| Octet(s) | Signification | Statut |
|---|---|---|
| `[0]` bit 7 | **électrode de saumure immergée** (1 = saumure OK, 0 = manque de sel). Vérifié en sortant / remettant la sonde. Pendant une régénération, tombe à 0 si la saumure est insuffisante | ✅ |
| `[0]` bit 0 | **régénération en cours** | ✅ |
| `[0]` bit 1, bit 2 | actifs pendant la régénération (phase / moteur ?) | 🔶 |
| `[0]` bit 6 | apparu quand le module Connectivity a été retiré (communication ?) | ❔ |
| `[1]`, `[2]` | progression / étape de la régénération (0 au repos) | 🔶 |
| `[4]` | compte à rebours horaire, 0 au repos | ❔ |
| `[6-7]` | **compteur de capacité**, u16 LE : +17 par litre (≈ dureté retirée en °f·L), **remis à 0 au début de chaque régénération**, régénération observée vers ~3000 | ✅ / 🔶 seuil |
| `[8]` | compteur d'impulsions du débitmètre | 🔶 |
| `[14-15]` | **secondes restantes de la phase de régénération**, u16 LE | ✅ |
| `[16-17]` | secondes écoulées dans la phase | 🔶 |
| `[21]` | minutes restantes de la phase | ✅ |
| `[18]`, `[20]`, `[22]`, `[24]` | variables pendant la régénération | ❔ |
| `[26]` | 14 au repos (= dureté résiduelle 14 °f ?), varie pendant la régénération | 🔶 |
| `[28-29]` | `0E 0A` (évolue rarement) | ❔ |
| `[30-31]` | **nombre total de régénérations**, u16 LE | ✅ |

### 5.2 Bloc B (page 0)

| Octet(s) | Signification | Statut |
|---|---|---|
| `[18-19]` | **débit instantané**, u16 LE, en **centilitres/min** (1998 = 19,98 L/min) ; non nul seulement pendant un puisage | ✅ |
| `[3]` | bascule 0x10 / 0x12 pendant la régénération | ❔ |
| `[28]` | 0x80…0x8E pendant la régénération | ❔ |
| autres | constantes au repos : `00 01 0F 10 … 4F 05 11 02 80 01` | ❔ |

### 5.3 Bloc C (page 2)

| Octet(s) | Signification | Statut |
|---|---|---|
| `[1]` | valeur décroissant lentement | ❔ |
| `[18]` | `0x80` | ❔ |

### 5.4 Historique (pages 3 et 4)

- **Page 3** : `[0]` = nombre d'entrées, puis des valeurs u16 (jours de fonctionnement auxquels le message est apparu).
- **Page 4** : paires (code, index). Codes observés : `0x15`, `0x16`, `0x19` (signification à déterminer).

---

## 6. Déroulement d'une régénération (observé)

| t | Bloc A `[0]` | Événement |
|---|---|---|
| 0 | `80 → 85` | début : bit 0 = 1, compteur de capacité remis à 0 |
| +1 min | `87` | première phase (~9 min), `[14-15]` / `[21]` décomptent |
| +6 min | `05` | **bit 7 = 0** : saumure non détectée pendant l'aspiration (sel insuffisant) |
| +10 min | `87` | deuxième phase (~11 min) |
| +17 min | `80` | fin : compteur de régénérations +1 |

Avec suffisamment de sel, le bit 7 doit rester à 1 pendant toute la régénération.

---

## 7. Cycle d'interrogation utilisé par l'ESP (mode maître)

| Requête | Période |
|---|---|
| `0x0002` puis `0x0001` | au démarrage, puis toutes les 5 min |
| `0x0043` page 1 (bloc A) | 2 s |
| `0x0029` (eau totale) | 5 s |
| `0x0043` pages 0 et 2 | 10 s |
| `0x0025`, `0x0026` | 60 s |
| autres commandes, pages 3 et 4 | 5 min |

Une requête au plus toutes les 300 ms, uniquement lorsque le bus est libre depuis 30 ms ;
attente de réponse 400 ms. Toute réponse `0x0043` arrivée hors délai est ignorée
(sinon la page ne correspond plus et un bloc B pourrait être lu comme un bloc A).

## Diagnostic : boîte noire et anti-collision (mode maître)

- Le composant garde en RAM les **32 derniers échanges** (T = trame émise par l'ESP, R = rafale reçue, horodatage et 24 premiers octets).
- À la première panne (10 requêtes consécutives sans réponse, ou bus muet en mode écoute), il les écrit dans le journal au niveau **ERROR** (`=== Boîte noire bus JUDO ... ===`). Une seule fois par incident ; le déclencheur est réarmé dès qu'une trame valide est reçue.
- Anti-collision : l'ESP n'émet qu'après 50 ms de silence sur le bus, jamais pendant la réception d'une trame partielle (< 300 ms). Après une requête restée sans réponse (400 ms), il attend 150 ms de plus avant de réémettre. Les réponses 0x0043 arrivées hors délai sont ignorées.
