# ESP32-S31 CAN-FD 프로젝트 위키 (LLM용)

다른 세션의 LLM이 이 저장소를 처음 볼 때 읽는 문서다.
구조, 동작 원리, 작업 환경, 주의 사항을 한곳에 모았다.
콘솔 명령 사용법은 [CONSOLE_COMMANDS.md](CONSOLE_COMMANDS.md)에 따로 정리했다.

> 기준 커밋: `b341262` (2026-10-28).
> `README.md`의 "부팅하면 두 포트를 연다"는 설명은 이전 내용이다. 지금은 MODE2 포트만 부팅 때 연다.

## 1. 한 줄 요약

ESP32-S31 보드 하나로 CAN-FD 2포트 시험기, USB-CAN 어댑터, OBD-II ECU 시뮬레이터를 겸한다.

- **콘솔 시험기**: USB Serial/JTAG 콘솔(`canfd>`)에서 두 포트를 직접 열고 송수신, 에러, 고장 주입을 시험한다.
- **USB-CAN 어댑터**: HS OTG 포트에서 Candlelight(gs_usb) 장치로 동작한다. cangaroo가 붙는다.
- **OBD-II ECU 시뮬레이터**: 포트별 MODE2에서 OBD 요청에 응답한다. cangaroo 모니터와 동시에 동작한다.

이 프로젝트는 프로토타입이다. 요청에 필요한 만큼만 바꾸고, 관계없는 수정이나 미래 대비 프레임워크는 넣지 않는다.

## 2. 하드웨어

| 항목 | 내용 |
|---|---|
| 보드 | ESP32-S31-Function-CoreBoard-1 (ESP32-S31-WROOM-3, 플래시 16 MB) |
| PSRAM | 16 MB Octal, 200 MHz, 힙에 추가 |
| CAN1 | TX GPIO35, RX GPIO36 |
| CAN2 | TX GPIO37, RX GPIO38 |
| 콘솔 | USB Serial/JTAG (PC에서 COM23). 플래시도 이 포트로 한다 |
| USB 어댑터 | HS OTG 포트. VID 0x303A, PID 0x4C31, 시리얼 = base MAC 12자리 |
| RGB LED | GPIO60 (WS2812). cangaroo의 identify 기능에 쓴다 |
| CAN 클럭 | 80 MHz (`CANFD_CLOCK_HZ`) |

보드에는 CAN 트랜시버가 없어서 포트마다 외부 트랜시버를 단다.
현재 시험 환경에서는 CAN1과 CAN2가 같은 버스로 연결되어 있다(종단 저항 포함).
그래서 한 포트로 보낸 프레임을 다른 포트가 받는다.

## 3. 작업 환경 (Windows)

### 빌드와 플래시

ESP-IDF v6.1을 쓴다. esp32s31은 프리뷰 타깃이다. `sdkconfig`는 이미 esp32s31로 맞춰져 있다.

| 항목 | 경로 |
|---|---|
| IDF | `C:\esp\v6.1\esp-idf` |
| 도구 | `C:\Espressif\tools` |
| Python venv | `C:\Espressif\tools\python\v6.1\venv` |

PowerShell에서 아래 환경 변수를 잡고 `idf.py`를 실행한다.
PATH에는 `C:\Espressif\tools` 아래의 cmake, ninja, riscv32-esp-elf, ccache 등의 bin 폴더를 넣는다.

```powershell
$env:IDF_PATH = "C:\esp\v6.1\esp-idf"
$env:IDF_TOOLS_PATH = "C:\Espressif\tools"
$env:IDF_PYTHON_ENV_PATH = "C:\Espressif\tools\python\v6.1\venv"
$env:ESPPORT = $null    # 시스템에 ESPPORT=COM4가 잡혀 있어 -p COM23과 충돌한다
& "C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe" "$env:IDF_PATH\tools\idf.py" build
& "C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe" "$env:IDF_PATH\tools\idf.py" -p COM23 flash
```

- `ESPPORT`가 `COM4`로 잡혀 있으면 `-p COM23`과 충돌해 플래시가 바로 실패한다. 먼저 지운다.
- COM4는 다른 보드(CP210x)다. 이 보드는 COM23이다.

### 콘솔 포트와 UART MCP

사용자의 UartTerminal 앱이 COM23을 열어 두고 MCP 서버(`uart`)로 공유한다.
LLM은 `mcp__uart__*` 도구로 콘솔을 쓴다. 열린 포트가 여러 개라서 `port: "COM23"`을 항상 넣는다.

플래시 순서는 다음과 같다.

1. `uart_close(port="COM23")`로 포트를 넘긴다.
2. 셸에서 `idf.py -p COM23 flash`를 실행한다.
3. `uart_open(port="COM23")`로 다시 연다.

`uart_reset(port="COM23")`은 보드를 리셋한다. 부팅 로그가 끝나면 `USB connected`가 찍힌다.

### 저장소 규칙

- 커밋과 push는 사용자가 요청할 때만 한다. 커밋 작성자는 `jyp`다.
- 커밋 메시지는 영어로 쓰고, 문서는 한국어로 쓴다.
- GUI(cangaroo) 저장소는 `C:\work\Tools\CAN_TerminalV2`다. 요청이 없으면 건드리지 않는다.

## 4. 소스 구조

| 파일 | 역할 |
|---|---|
| `main/app_main.c` | 콘솔 REPL 시작, 명령 등록, `ch_mode_init()`, `candle_usb_init()` |
| `main/canfd_port.c/h` | TWAI-FD 드라이버 래퍼. TX 프레임 풀, RX 이벤트 큐, 포트별 RX 태스크, 고장 주입 |
| `main/cmd_canfd.c/h` | `can_*` 콘솔 명령과 `can_test` |
| `main/cmd_system.c/h` | `mem` 명령 (힙, PSRAM 시험) |
| `main/ch_mode.c/h` | 포트별 모드(MODE0/1/2) 저장과 적용. MODE2 포트의 훅을 맡는다 |
| `main/obd_ecu.c/h` | OBD-II ECU 시뮬레이터 (서비스 01, 03, 04, 09) |
| `main/cmd_mode.c` | `mode`, `obd` 콘솔 명령 |
| `main/candle.c/h` | Candlelight / ElmueSoft 프로토콜 엔진 (요청 처리, 프레임 포장, 에러 보고) |
| `main/candle_def.h` | 프로토콜 구조체와 상수 (CANable 2.5 펌웨어와 같은 정의) |
| `main/candle_usb.c/h` | TinyUSB 디스크립터, MS OS 2.0 (WinUSB), `usb` 명령 |
| `main/Kconfig.projbuild` | 핀, 기본 비트레이트, 큐 깊이, USB PID, 호스트 FIFO 깊이 |
| `docs/can_bus_error_test.html` | 버스 에러 시험 보고서 |

## 5. 태스크 구성

| 태스크 | 우선순위 | 스택 | 코어 | 하는 일 |
|---|---|---|---|---|
| `can1_rx`, `can2_rx` | 10 | 4 KB | 고정 안 함 | RX 이벤트 처리, 훅 호출, 에러 로그, 자동 복구 |
| `candle` | 6 | 4 KB | 고정 안 함 | 5 ms 주기. 에러 보고, TX 타임아웃, 버스 부하, LED |
| TinyUSB | 5 | 4 KB | CPU1 | SETUP 요청, CAN TX(호스트 → 버스), IN 전송 완료 |
| 콘솔 REPL | 2 | 8 KB | 고정 안 함 | 콘솔 명령 실행 |

- RX 태스크는 포트를 열 때 만들고 닫을 때 지운다.
- OBD ECU 응답은 해당 포트의 RX 태스크 안에서 보낸다. 송신 대기는 최대 10 ms다.
- 호스트 프레임 송신은 TinyUSB 태스크에서 빈 TX 슬롯을 최대 20 ms 기다린다.

## 6. 포트 소유권과 데이터 흐름

`canfd_port`는 포트마다 RX 훅 하나와 TX 훅 하나만 가진다. 누가 훅을 거느냐가 곧 포트의 주인이다.

### 포트 모드 (NVS 저장, 기본 MODE0)

| 모드 | 이름 | 동작 |
|---|---|---|
| 0 | off | 부팅 때 열지 않는다. cangaroo가 열려고 하면 거부한다 |
| 1 | USB adapter | cangaroo가 열고 닫는다. 이전 동작과 같다 |
| 2 | USB adapter + OBD ECU | ECU가 항상 동작한다. cangaroo도 같은 포트를 쓴다 |

### MODE2 흐름

- 호스트가 없을 때: `ch_mode`가 포트를 500 kbit/s 클래식으로 열고 훅을 건다.
- cangaroo가 채널을 시작할 때: 호스트 설정으로 포트를 다시 열고 같은 훅을 건다.
  호스트 설정으로 열지 못하면 500 kbit/s로 되돌아간다.
- cangaroo가 채널을 멈추거나 USB가 끊길 때: 500 kbit/s로 다시 연다. ECU는 계속 동작한다.

훅이 프레임을 나누는 방식은 다음과 같다.

```text
버스 수신 ─► obd_rx_hook ─┬─► candle_port_rx()   (호스트에 RX 프레임으로 보고)
                          └─► obd_ecu_rx()       (요청이면 응답 송신)

TX 완료 ─► obd_tx_hook ─┬─ tag == OBD_ECU_TX_TAG ─► 성공이면 candle_port_rx()
                        │                           (ECU 응답이 cangaroo에 RX로 보임)
                        └─ 그 밖 (호스트 프레임) ─► candle_port_tx_done()  (TX 에코)
```

- ECU 프레임의 tag는 `0xFFFFFFFE`다. 호스트 echo_id나 Elmue 마커와 겹치지 않는다.
- ECU 응답의 타임스탬프는 마지막 수신 프레임의 노드 타임스탬프를 기준으로 맞춘다.

### MODE1 / 콘솔 흐름

- MODE1 포트는 `candle.c`의 `ch_open()`이 직접 `on_rx_frame`, `on_tx_done` 훅을 건다.
- 콘솔 `can_init`은 모드와 관계없이 포트를 연다. 훅은 걸지 않는다. 대신 `can_dump`로 출력한다.

## 7. USB Candlelight 어댑터

- HS(480 Mbit/s)로 붙는다. `usb` 명령에서 속도를 확인한다.
- 인터페이스 배치는 CANable 2.5 펌웨어와 같다.
  - 인터페이스 0 = CAN1, 인터페이스 1 = 펌웨어 업데이트(DFU runtime), 인터페이스 2 = CAN2
  - TinyUSB vendor 인스턴스 번호(`itf`)는 0 = CAN1, 1 = CAN2다.
- WinUSB가 자동으로 붙도록 MS OS 2.0 디스크립터를 넣었다.
- 프로토콜은 두 가지다. 모든 채널이 닫혀 있을 때만 바꿀 수 있다.
  - legacy gs_usb: 모든 채널이 인터페이스 0을 쓴다. 약 5,600 frames/s까지 시험했다.
  - ElmueSoft (CANable 2.5): 채널마다 자기 인터페이스를 쓴다. blob 전송으로 16,600 frames/s를 손실 없이 시험했다.
- 비트 타이밍: cangaroo는 레지스터 값이 아니라 tq 수(brp, prop, ph1, ph2, sjw)를 보낸다.
  펌웨어가 TWAI-FD 레지스터 범위에 맞춰 넣는다. cangaroo는 80 MHz 표를 쓴다.
- 알려진 제약: 펌웨어가 요청을 거절해도 DATA 단계에서 true를 반환한다. 그래서 cangaroo에는 오류가 보이지 않는다.
  MODE0 포트를 cangaroo가 열면 열린 것처럼 보이지만 프레임이 오지 않는다.
- 호스트가 연 채널에서 TX가 500 ms 동안 ACK를 받지 못하면 포트를 다시 열어 대기 프레임을 버린다.

## 8. OBD-II ECU 시뮬레이터

### 주소

- 기능 주소 `7DF`와 설정한 물리 주소(기본 `7E0`)의 요청에 응답한다.
- 응답 ID는 물리 주소 + 8이다(기본 `7E8`).
- 11비트 클래식 CAN만 처리한다. 응답은 8바이트이고 남는 바이트는 `0xAA`로 채운다.

### 지원 서비스

| 요청 | 응답 | 비고 |
|---|---|---|
| `01 00/20/40/60/80/A0` | Supported PID 비트맵 | 켜진 항목에서 계산한다. 비트가 0인 범위(00 제외)는 응답하지 않는다 |
| `01 05` | A = 온도 + 40 | 냉각수 온도 [°C] |
| `01 0C` | rpm × 4 (2바이트) | 엔진 회전수 |
| `01 0D` | km/h | 차속 |
| `01 1F` | 초 (2바이트) | 부팅 후 경과 시간. 설정할 수 없다 |
| `01 2F` | % × 255 / 100 | 연료 잔량 |
| `01 A6` | 0.1 km 단위 (4바이트) | 적산 주행거리 |
| `09 00` | `40 00 00 00` | PID 02만 지원 |
| `09 02` | `49 02 01` + VIN 17자 | 멀티프레임 (FF + CF 2개) |
| `03` | `43 N` + DTC × 2바이트 | 저장된 DTC. 0개면 `43 00` |
| `04` | `44` | 이후 03에 `43 00`으로 응답한다. 재부팅하거나 목록을 다시 설정하면 되살아난다 |

- 한 요청에 PID가 여러 개 있어도 첫 PID에만 응답한다.
- 지원하지 않는 요청에는 응답하지 않는다(NRC 없음).

### ISO-TP 멀티프레임

- 7바이트를 넘는 응답은 FF를 먼저 보내고, 테스터의 FC(`30`)를 받은 뒤 CF를 모두 보낸다.
- FC는 물리 주소(기본 `7E0`)로 와야 한다. FF 뒤 1초가 지난 FC는 무시한다.
- FC의 BS와 STmin은 적용하지 않는다. CF를 연달아 보낸다.
- 송신 버퍼는 20바이트다(VIN 20바이트, DTC 8개 18바이트).

### 기본값

| 항목 | 기본값 |
|---|---|
| 요청 주소 | `7E0` |
| 켜진 항목 | 전부 (05 0C 0D 1F 2F A6 VIN DTC) |
| 냉각수 온도 | 90 °C |
| rpm | 800 |
| 차속 | 0 km/h |
| 연료 | 75 % |
| 주행거리 | 12345 km |
| VIN | `KMHESP32S31000001` |
| DTC | 없음 |

## 9. NVS 저장 구조

네임스페이스는 `hybrid`다.

| 키 | 형식 | 내용 |
|---|---|---|
| `mode1`, `mode2` | u8 | 포트 모드 (0/1/2) |
| `obd1`, `obd2` | blob | `obd_ecu_cfg_t` 구조체 그대로 |

- blob 크기가 구조체 크기와 다르면 기본값을 쓴다. 구조체를 바꾸면 기존 OBD 설정은 한 번 기본값으로 돌아간다.
- 버전 관리는 하지 않는다(프로토타입).
- 콘솔에서 `mode`나 `obd`로 값을 바꾸면 그 자리에서 저장한다.

## 10. 주의 사항

- **부팅 직후 첫 콘솔 입력이 깨진다.** 첫 명령이 "Unrecognized command"로 끝난다. 빈 줄을 먼저 한 번 보낸다.
- **MODE2 포트에 콘솔 `can_init`을 쓰지 않는다.** 포트를 다시 열면서 ECU 훅이 빠진다. 복구하려면 `mode <port> 2`를 다시 실행한다.
- **MODE2 포트를 `can_test`의 수신 쪽으로 쓰지 않는다.** 시험이 끝나면 수신 포트의 RX 훅을 지워서 ECU가 빠진다.
  기본 방향(`both`, `dual`)은 두 포트 모두 수신 쪽이 된다. 복구 방법은 위와 같다.
- **MODE2 포트에서는 `can_dump`가 출력하지 않는다.** 훅이 걸린 포트는 덤프를 건너뛴다. 프레임은 다른 포트나 cangaroo로 본다.
- **cangaroo가 연 채널의 모드는 바꿀 수 없다.** `mode` 명령이 거부한다. cangaroo에서 먼저 멈춘다.
- **cangaroo가 Listen-only로 열면** 노드가 송신하지 못하므로 ECU도 응답하지 못한다.
- 기본 재전송 한도는 무제한이다. 상대 노드가 없으면 같은 프레임을 계속 다시 보낸다.
- `CANFD_AUTO_INIT` Kconfig는 지웠다. 부팅 때 두 포트를 모두 여는 동작은 없다.

## 11. 확장 방법 (WWH-OBD, J1939, 확장 ID)

새 기능은 OBD처럼 파일을 하나 더 붙이는 정도로 넣는다.

1. `ch_mode.h`의 `ch_mode_t`에 모드 값을 추가한다.
2. 새 파일(예: `j1939_ecu.c`)에 `xxx_rx(port, msg)`와 설정 저장 함수를 만든다.
3. `ch_mode.c`의 RX 훅에서 모드에 따라 새 함수를 부른다. 응답 프레임에는 고유 tag를 붙인다.
4. `candle.c`는 `ch_mode_get() == CH_MODE_OBD`로 분기한다. 새 모드도 같은 분기에 넣는다.
5. 콘솔 명령은 `cmd_mode.c`에 추가한다.

## 12. 커밋 이력

| 커밋 | 내용 |
|---|---|
| `9acfc96` | CAN-FD 2포트 시험 + 콘솔 |
| `cb7ce5d` | 16 MB PSRAM, CAN 큐를 PSRAM으로 |
| `53916ad` | HS OTG USB Candlelight 어댑터 (legacy + ElmueSoft) |
| `6136055` | CANable 2.5와 같은 인터페이스 배치, BOOT0 핀 상태 |
| `f48bffa` | 포트별 모드, OBD-II ECU 시뮬레이터 (하이브리드) |
| `b341262` | OBD DTC: 서비스 03, 04 |
