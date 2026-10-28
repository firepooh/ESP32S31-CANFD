# 콘솔 명령 레퍼런스 (시험 도구용)

ESP32-S31 보드를 CAN 시험 도구로 쓸 때 보는 문서다.
다른 세션에서 이 문서만 읽고 바로 쓸 수 있게 썼다.
프로젝트 구조와 동작 원리는 [LLM_WIKI.md](LLM_WIKI.md)를 본다.

## 0. 바로 쓰기

### 연결 정보

| 항목 | 값 |
|---|---|
| 콘솔 포트 | COM23 (USB Serial/JTAG), 115200 8N1 |
| 프롬프트 | `canfd> ` |
| CAN1 / CAN2 | 같은 버스에 연결됨. 한 포트로 보낸 프레임을 다른 포트가 받는다 |
| 포트 번호 | `1`, `2`, `all`. `can1`, `can2`도 된다 |
| ID와 데이터 | 16진수. 데이터는 `1122AABB`, `11.22.AA.BB`, `11:22:AA:BB` 모두 된다 |

### LLM이 UART MCP로 쓰는 방법

COM23은 사용자의 UartTerminal이 열어 두고 MCP(`uart`)로 공유한다.
다른 포트도 열려 있으므로 모든 호출에 `port: "COM23"`을 넣는다.

1. `uart_status(port="COM23")`로 `end_cursor`를 얻는다.
2. `uart_send(port="COM23", text="명령")`으로 명령을 보낸다.
3. `uart_expect(port="COM23", cursor=<1에서 얻은 값>, pattern="canfd> $")`로 응답을 기다린다.

`uart_expect`에 cursor를 넘기지 않으면 호출 뒤에 도착한 데이터만 본다. 응답이 먼저 와 버리면 놓친다.
그래서 보내기 전에 cursor를 잡아 두고 넘긴다.
여러 명령을 이어서 보낼 때는 이전 `uart_expect`가 돌려준 cursor를 다음 호출에 넘긴다.

### 부팅 직후 주의

리셋 뒤 첫 콘솔 입력은 "Unrecognized command"로 깨진다.
빈 줄(`uart_send(text="")`)을 먼저 한 번 보내고 프롬프트를 기다린 뒤 명령을 보낸다.

## 1. 시험 구성 레시피

### A. 보드 하나로 OBD 시험 (CAN1 = ECU, CAN2 = 테스터)

```text
mode 1 2                        # CAN1을 ECU로 (NVS 저장, 한 번만 하면 된다)
can_init 2 -b 500000 -c         # CAN2를 500 kbit/s 클래식으로 연다
can_dump 2                      # CAN2가 받은 프레임을 출력한다
can_send 2 7DF 02010C0000000000 # RPM 요청
```

예상 출력은 다음과 같다.

```text
CAN2: queued 1, sent 1, failed 0  (ERROR_ACTIVE TEC=0 REC=0)
CAN2    60.457420       7E8            [ 8]  04 41 0C 0C 80 AA AA AA
```

- CAN2가 MODE0 상태여도 콘솔 `can_init`으로 열 수 있다. 모드는 USB와 부팅 동작에만 영향을 준다.
- 응답은 CAN2 덤프로 본다. CAN1(MODE2)에서는 덤프가 출력되지 않는다.

### B. 외부 스캔툴이나 다른 노드로 OBD 시험

```text
mode 1 2      # 또는 mode 2 2
obd 1         # 설정 확인
```

ECU는 500 kbit/s 클래식으로 응답한다. 외부 장비만 버스에 물리면 된다.

### C. cangaroo로 보면서 OBD 시험

1. `mode 1 2`로 CAN1을 ECU로 둔다.
2. cangaroo에서 ESP32 어댑터의 CAN1 채널만 켜고 500 kbit/s로 측정을 시작한다.
3. CAN2(콘솔)나 외부 장비로 요청을 보낸다.

cangaroo에는 요청, ECU 응답, FC, CF가 모두 RX로 보인다. cangaroo에서 보낸 프레임도 버스로 나간다.
cangaroo가 측정을 멈추면 ECU는 500 kbit/s로 돌아가서 계속 응답한다.

### D. 두 포트 전송 시험 (OBD와 무관)

```text
mode 1 1
mode 2 1                        # MODE2 포트가 있으면 먼저 내린다
can_test                        # CAN1→CAN2, CAN2→CAN1 순서대로
can_test -D dual -n 1000        # 양방향 동시
```

`can_test`는 닫힌 포트를 기본 설정(500 kbit/s / 2 Mbit/s FD)으로 연다.

## 2. 모드와 OBD 명령

### `mode`

```text
mode                    # 두 포트의 모드를 보여 준다
mode <port> <0|1|2>     # 모드를 바꾸고 NVS에 저장한다
```

| 값 | 의미 |
|---|---|
| 0 | off. 부팅 때 열지 않고, cangaroo의 열기 요청도 거부한다 (기본값) |
| 1 | USB 어댑터 전용. cangaroo가 열고 닫는다 |
| 2 | USB 어댑터 + OBD ECU. ECU가 항상 동작한다 |

- MODE2로 바꾸면 그 자리에서 포트를 500 kbit/s로 열고 ECU를 붙인다.
- MODE0이나 MODE1로 바꾸면 열려 있던 포트를 닫는다.
- cangaroo가 연 채널은 바꿀 수 없다: `CAN1 is started by the USB host, stop it in cangaroo first`

출력 예는 다음과 같다.

```text
canfd> mode 1 2
I (36372) ch_mode: CAN1: OBD ECU running at 500000 bps
CAN1: mode 2 (USB adapter + OBD ECU), saved
```

### `obd`

모든 변경은 바로 NVS에 저장되고, 바뀐 설정 전체를 출력한다.

```text
obd [port]                          # 설정 보기 (port를 빼면 두 포트 모두)
obd <port> addr <hex>               # 물리 요청 주소. 응답 = 요청 + 8. 000~7F7, 7DF 제외
obd <port> on|off <item|all>        # 항목 응답 켜기/끄기
obd <port> set <item> <value>       # 값 설정
obd <port> vin <17자>               # VIN
obd <port> dtc <코드 ...>|none      # 저장 DTC (최대 8개)
```

| item | 내용 | `set` 범위 |
|---|---|---|
| `05` | 냉각수 온도 [°C] | -40~215 |
| `0C` | 엔진 회전수 [rpm] | 0~16383 |
| `0D` | 차속 [km/h] | 0~255 |
| `1F` | 부팅 후 경과 시간 [s] | 설정 불가 (자동 증가) |
| `2F` | 연료 잔량 [%] | 0~100 |
| `A6` | 적산 주행거리 [km] | 0~429496729 |
| `VIN` | 09 00, 09 02 응답 | `obd <port> vin`으로 설정 |
| `DTC` | 03, 04 응답 | `obd <port> dtc`로 설정 |

- 범위를 벗어난 값은 가장 가까운 끝값으로 맞춘다.
- 항목을 끄면 Supported PID 비트에서도 빠진다.
- DTC 코드는 `P0301`, `C0123`, `B1234`, `U0100` 형식이다. 문자 다음 첫 숫자는 0~3이다.

출력 예는 다음과 같다.

```text
canfd> obd 1 dtc P0301 P0420 C0123
CAN1 OBD ECU: request 7E0 / 7DF, response 7E8
  05  on   coolant temperature [C]          90
  0C  on   engine speed [rpm]               800
  0D  on   vehicle speed [km/h]             0
  1F  on   run time since start [s]         19
  2F  on   fuel level [%]                   75
  A6  on   odometer [km]                    12345
  VIN on   vehicle identification number    KMHESP32S31000001
  DTC on   stored DTCs (03), clear (04)     P0301 P0420 C0123
```

04로 DTC를 지운 상태이면 DTC 줄 끝에 `(cleared by 04 until reboot)`가 붙는다.

## 3. OBD 요청 치트시트

테스터 포트가 CAN2이고 ECU 주소가 기본값(7E0/7E8)일 때의 예다.
요청 첫 바이트는 ISO-TP 단일 프레임 길이다.

| 하고 싶은 일 | 보낼 명령 | 기대 응답 (7E8) |
|---|---|---|
| Supported 01~20 | `can_send 2 7DF 0201000000000000` | `06 41 00 08 18 00 03 AA` |
| Supported 21~40 | `can_send 2 7DF 0201200000000000` | `06 41 20 00 02 00 01 AA` |
| Supported A1~C0 | `can_send 2 7DF 0201A00000000000` | `06 41 A0 04 00 00 00 AA` |
| 냉각수 온도 | `can_send 2 7DF 0201050000000000` | `03 41 05 82 ...` (0x82 - 40 = 90 °C) |
| 엔진 회전수 | `can_send 2 7DF 02010C0000000000` | `04 41 0C 0C 80 ...` (0x0C80 / 4 = 800 rpm) |
| 차속 | `can_send 2 7DF 02010D0000000000` | `03 41 0D 00 ...` |
| 경과 시간 | `can_send 2 7DF 02011F0000000000` | `04 41 1F 00 3C ...` (60 s) |
| 연료 잔량 | `can_send 2 7DF 02012F0000000000` | `03 41 2F BF ...` (0xBF × 100 / 255 = 75 %) |
| 주행거리 | `can_send 2 7DF 0201A60000000000` | `06 41 A6 00 01 E2 3A AA` (123450 × 0.1 km) |
| 물리 주소로 요청 | `can_send 2 7E0 02010C0000000000` | 7DF와 같은 응답 |
| 09 지원 PID | `can_send 2 7DF 0209000000000000` | `06 49 00 40 00 00 00 AA` |
| VIN | `can_send 2 7DF 0209020000000000` | FF `10 14 49 02 01 4B 4D 48` |
| VIN 나머지 (FC) | `can_send 2 7E0 3000000000000000` | CF `21 ...`, `22 ...` |
| 저장 DTC 읽기 | `can_send 2 7DF 0103000000000000` | 0개 `02 43 00`, 1~2개 단일 프레임, 3개 이상 FF |
| DTC 나머지 (FC) | `can_send 2 7E0 3000000000000000` | CF `21 ...` (필요하면 `22 ...`) |
| DTC 지우기 | `can_send 2 7DF 0104000000000000` | `01 44 AA ...` |

- FC는 반드시 물리 주소(7E0)로 보낸다. FF 뒤 1초 안에 보낸다.
- 지원하지 않는 PID, 꺼진 항목, 다른 주소(예: 7E1)에는 응답하지 않는다.
- DTC 2바이트 해석: 위 2비트가 문자(00 = P, 01 = C, 10 = B, 11 = U)이고 나머지 14비트가 숫자다.
  예: `41 23` = C0123, `C1 00` = U0100.

## 4. CAN 포트 명령

### `can_init`

```text
can_init [port] [-b bps] [-d bps] [-c] [--sp ‰] [--dsp ‰] [--dssp ‰] [-r n] [-a ms] [-m mode]
```

포트를 열거나 다시 연다. port를 빼면 두 포트 모두 연다. 기본값은 500 kbit/s / 2 Mbit/s다.

| 옵션 | 뜻 |
|---|---|
| `-b` | 공칭(중재) 비트레이트 |
| `-d` | 데이터 구간 비트레이트 (FD) |
| `-c` | 클래식 CAN만 사용 |
| `--sp`, `--dsp` | 공칭/데이터 샘플 포인트 [‰]. 0이면 드라이버 기본값 |
| `--dssp` | 데이터 구간 보조 샘플 포인트 (TDC) [‰] |
| `-r` | 재전송 한도 -1~15. -1이 무제한(기본) |
| `-a` | 버스 오프 뒤 자동 복구까지의 시간 [ms]. 0이면 수동 |
| `-m` | `normal`, `listen`, `selftest` |

MODE2 포트에 쓰면 ECU가 빠진다. 다시 붙이려면 `mode <port> 2`를 실행한다.

### `can_deinit`

```text
can_deinit [port]
```

포트를 닫는다.

### `can_send`

```text
can_send <port> <id> [data] [-e] [-f] [-b] [-r] [-l n] [-n n] [-p ms]
```

| 옵션 | 뜻 |
|---|---|
| `-e` | 29비트 확장 ID. ID가 0x7FF보다 크면 자동으로 확장 ID |
| `-f` | CAN-FD 프레임 (최대 64바이트) |
| `-b` | BRS. `-f`가 자동으로 켜진다 |
| `-r` | 리모트 프레임 (클래식 전용). `-l`이 요청 DLC |
| `-l` | 데이터 길이. 데이터를 빼면 00 01 02 ... 패턴으로 채운다 |
| `-n`, `-p` | 보낼 개수, 프레임 간격 [ms] |

송신이 끝나면 결과를 한 줄로 출력한다.

```text
CAN2: queued 1, sent 1, failed 0  (ERROR_ACTIVE TEC=0 REC=0)
```

- `sent`가 0이고 TX가 끝나지 않으면 ACK를 준 노드가 없는 것이다. 배선을 확인한다.
- 재전송이 무제한이므로 ACK가 없으면 같은 프레임을 계속 보낸다. `can_init <port>`로 비운다.

### `can_dump`

```text
can_dump [port] [on|off]
```

받은 프레임을 출력한다. 형식은 다음과 같다.

```text
CAN2    74.815462       7E8            [ 8]  10 14 49 02 01 4B 4D 48
포트    타임스탬프[s]    ID   FD/BRS/ESI  [길이]  데이터
```

훅이 걸린 포트(MODE2 포트, cangaroo가 연 포트)는 출력하지 않는다.

### `can_status`

```text
can_status [port] [-c]
```

설정, 상태(ERROR_ACTIVE/WARNING/PASSIVE/BUS_OFF), TEC/REC, 송수신 통계, 에러 종류별 개수, 큐 사용량을 보여 준다.
`-c`는 출력 뒤 통계를 지운다.

### `can_recover`

```text
can_recover [port] [-t ms]
```

버스 오프 상태의 포트를 복구한다. 기본 대기 시간은 2000 ms다.

### `can_errlog`

```text
can_errlog [port] [on|off]
```

에러가 나는 동안 500 ms마다 출력하는 에러 요약 로그를 켜거나 끈다. 기본은 on이다.

### `can_fault`

```text
can_fault <port> <dominant|recessive|off>
```

TX 핀을 컨트롤러에서 떼어 고정한다. 닫힌 포트에도 걸 수 있다.

| 값 | 핀 상태 | 흉내 내는 고장 |
|---|---|---|
| `dominant` | TX Low 고정 | 노드가 버스를 우성으로 붙잡음 |
| `recessive` | TX High 고정 | TX 선 단선 |
| `off` | 컨트롤러에 반환 | 정상 |

트랜시버의 우성 타임아웃 때문에 `dominant`로 버스를 계속 붙잡지는 못한다.

### `can_test`

```text
can_test [-D dir] [-n n] [-l n] [-i hex] [-e] [-c] [--nobrs] [-g ms] [-w us] [-t ms]
```

송신 포트가 일련번호가 든 프레임을 연속으로 보내고, 수신 포트가 순서와 데이터를 검사한다.

| `-D` | 동작 |
|---|---|
| `both` (기본) | CAN1→CAN2 뒤 CAN2→CAN1 |
| `dual` | 두 방향 동시 |
| `12`, `21` | 한 방향만 |
| `11`, `22` | 한 포트 루프백 (`can_init <port> -m selftest` 필요) |

| 옵션 | 뜻 |
|---|---|
| `-n` | 방향당 프레임 수 (기본 100) |
| `-l` | 데이터 길이 (기본 FD 64, 클래식 8) |
| `-i` | CAN1→CAN2 ID (기본 100). 반대 방향은 ID+1 |
| `-e`, `-c`, `--nobrs` | 확장 ID, 클래식 프레임, BRS 없는 FD |
| `-g` | 프레임 간격 [ms] |
| `-w` | 수신 쪽을 일부러 늦춤 [µs] (RX 큐 채우기) |
| `-t` | 수신 대기 시간 [ms] |

마지막 줄이 `=== can_test both: PASS ===`이면 통과다.
수신 포트로 쓴 MODE2 포트는 시험 뒤 ECU가 빠진다. `mode <port> 2`로 다시 붙인다.

## 5. 기타 명령

| 명령 | 기능 |
|---|---|
| `usb` | USB 연결 상태, 속도(high/full), 프로토콜(legacy/ElmueSoft), 채널 상태, IN FIFO 사용량 |
| `mem` | 내부 RAM, PSRAM 사용량 |
| `mem -t <KB>` | PSRAM 쓰기/검증 시험 |
| `mem -b <KB>` / `mem -s` | PSRAM 백그라운드 부하 시작 / 멈추고 결과 출력 |
| `help [cmd]` | 명령 도움말 |

## 6. 문제 해결

| 증상 | 원인과 조치 |
|---|---|
| 첫 명령이 `Unrecognized command` | 부팅 직후 입력이 깨지는 현상. 빈 줄을 먼저 보낸다 |
| ECU가 응답하지 않음 | `mode`로 MODE2인지 확인. `can_init`이나 `can_test` 뒤라면 `mode <port> 2`로 다시 붙인다 |
| 응답이 덤프에 안 보임 | MODE2 포트를 덤프하고 있다. 테스터 포트(예: CAN2)를 덤프한다 |
| VIN이나 DTC의 CF가 안 옴 | FC를 7DF로 보냈거나 1초가 지났다. 7E0으로 `30 00 00`을 바로 보낸다 |
| `mode` 변경이 거부됨 | cangaroo가 그 채널을 쓰고 있다. cangaroo에서 측정을 멈춘다 |
| `can_send`에서 `sent 0` | 상대 노드의 ACK가 없다. 상대 포트가 열려 있는지, 비트레이트가 같은지 확인한다 |
| 플래시가 COM23을 못 엶 | UartTerminal이 포트를 쥐고 있다. `uart_close` 뒤 플래시하고 `uart_open`으로 다시 연다 |
