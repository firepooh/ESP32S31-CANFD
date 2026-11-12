ESP32-S31-Function-CoreBoard-1 CAN-FD 2포트 시험기, USB-CAN 어댑터, OBD-II ECU 시뮬레이터 펌웨어 {VER}

- 빌드: ESP-IDF {IDF_VERSION} (`espressif/idf:{IDF_VERSION}` Docker 이미지), 대상 `{IDF_TARGET}`
- 커밋: {SHA}

## 파일

| 파일 | 내용 |
|---|---|
| `{BASE}-merged.bin` | 부트로더, 파티션 테이블, 앱을 합친 단일 이미지. 0x0에 쓴다. |
| `{BASE}-all.zip` | 개별 바이너리, `flash_args`, `.elf`, `sdkconfig` |
| `SHA256SUMS.txt` | 위 두 파일의 SHA-256 |

## 굽는 방법

**웹 플래셔:** {FLASHER_URL}

데스크톱 Chrome이나 Edge에서 연다. 보드의 USB-UART(CP210x) 포트를 PC에 연결하고, 버전을 고른 뒤 연결과 플래시를 차례로 누른다.

**esptool, 병합 이미지:**

```
esptool --chip {IDF_TARGET} -p <PORT> -b 460800 --no-stub write-flash 0x0 {BASE}-merged.bin
```

**esptool, 개별 파일:** `{BASE}-all.zip`을 풀고 그 폴더에서 실행한다.

```
esptool --chip {IDF_TARGET} -p <PORT> -b 460800 --no-stub write-flash "@flash_args"
```

- esptool은 이 칩을 지원하는 버전이어야 한다. IDF {IDF_VERSION}에 들어 있는 esptool을 쓰면 된다.
- `--no-stub`은 IDF가 이 칩을 구울 때 쓰는 옵션과 같다 (sdkconfig `CONFIG_ESPTOOLPY_NO_STUB=y`).
