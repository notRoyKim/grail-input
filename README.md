# grail-input

Grail(Sokpop)에 **게임패드와 키보드 조작**을 더하는 모드입니다. Windows와 **스팀덱(Proton)** 에서 동작합니다.

- 스틱으로 커서를 움직이고 A로 클릭·드래그
- **D패드 / 방향키로 버튼과 카드 사이를 바로 점프** — 게임이 마우스 판정에 쓰는 영역을 그대로 읽어서, 화면에 실제로 있는 버튼·카드·지도 칸으로만 이동합니다
- 게임 파일은 건드리지 않습니다. DLL 하나(`version.dll`)와 설정 파일 하나만 게임 폴더에 넣으면 되고, 지우면 원래대로 돌아갑니다
- Parsec 원격 플레이 때 마우스가 창에 갇히는 문제를 고치는 옵션 포함

> 비공식 팬 모드입니다. Sokpop과 관계없습니다. 게임 업데이트로 동작이 바뀔 수 있습니다.

## 설치

[Releases](../../releases)에서 `grail-input-<버전>.zip`을 받습니다.

### Windows

1. Steam 라이브러리에서 Grail 우클릭 → **관리 → 로컬 파일 보기**
2. zip 안의 `version.dll`, `grail_input.ini`를 `grail.exe`가 있는 폴더에 복사
3. 게임 실행. 같은 폴더에 `grail_input.log`가 생기면 정상 적용된 것입니다

PlayStation·Switch Pro 패드는 Steam 설정 → 컨트롤러에서 해당 패드의 **Steam Input**을 켜 두면 Xbox 패드처럼 인식됩니다.

### 스팀덱 (Proton)

1. 데스크톱 모드에서 Steam → Grail → **관리 → 로컬 파일 보기** (보통 `~/.local/share/Steam/steamapps/common/card-battler`)
2. `version.dll`, `grail_input.ini`를 `grail.exe` 옆에 복사
3. Grail **속성 → 일반 → 실행 옵션**에 아래 한 줄 입력 (Proton이 이 DLL을 쓰게 하는 설정):
   ```
   WINEDLLOVERRIDES="version=n,b" %command%
   ```
4. 게임 모드로 돌아가 Grail의 **컨트롤러 설정 → 레이아웃을 "게임패드"** (또는 "마우스 트랙패드가 있는 게임패드")로 선택
5. 실행. 터치스크린과 트랙패드도 그대로 함께 쓸 수 있습니다

### 제거

`version.dll`을 지우면 끝입니다. (`grail_input.ini`, `grail_input.log`도 지워도 됩니다. 스팀덱은 실행 옵션도 지우세요.)

## 기본 조작

| 게임패드 | 키보드 | 동작 |
|---|---|---|
| 왼쪽 스틱 | Shift + 방향키 | 커서 자유 이동 |
| D패드 | 방향키 | 그 방향의 가장 가까운 버튼·카드로 점프 (누르고 있으면 반복) |
| A | Enter, Space | 클릭 (누르고 있으면 드래그) |
| B | 오른쪽 Ctrl, 메뉴 키 | 오른쪽 클릭 |
| X, START | (Esc 그대로) | Esc — 메뉴 열기·창 닫기 |
| LB / RB, 오른쪽 스틱 | PageUp / PageDown | 스크롤 |
| LT 누르고 있기 | | 커서 느리게 (정밀 조작) |
| RT 누르고 있기 | | 커서 빠르게 |
| BACK | Home | 커서를 화면 가운데로 |

Y 버튼은 비워 두었습니다(게임 자체가 일부 화면에서 패드 입력을 읽습니다). 모든 버튼·키는 `grail_input.ini`에서 바꿀 수 있습니다.

실제 마우스를 움직이면 바로 마우스 조작으로 돌아오고, 패드나 키를 누르면 그 위치에서 이어서 움직입니다.

## 설정 (`grail_input.ini`)

파일 안에 한국어/영어 설명이 있습니다. 자주 쓰는 것:

- `[gamepad] cursor_speed` — 스틱 커서 속도
- `[navigation] smart = 0` — D패드로 점프하지 않고 조금씩만 움직이기
- `[navigation] ignore_scripts` — 점프 대상에서 뺄 게임 스크립트 목록. 반응 없는 곳으로 간다면 `log_level = 2`로 로그를 보고 해당 스크립트 이름을 추가하세요
- `[cursor] parsec_fix = transparent` — Parsec으로 원격 플레이할 때 마우스가 창에 갇히는 문제 해결 (예전 `parsec_cursor_fix`와 같은 기능, 이 모드에 포함됨)
- `[general] log_level = 2` — 문제가 있을 때 자세한 로그

## 문제 해결

- **아무 반응이 없음**: 게임 폴더에 `grail_input.log`가 생겼는지 확인하세요. 없으면 DLL이 로드되지 않은 것입니다(스팀덱은 실행 옵션 확인).
- **D패드 점프가 안 되고 조금씩만 움직임**: 로그의 `hooks:` 줄을 확인하세요. 게임 업데이트로 내부 이름이 바뀌면 `smart navigation UNAVAILABLE`이 뜨고, 이때도 커서 조작은 됩니다.
- **패드 인식 안 됨**: 로그에 `gamepad 0 connected`가 있는지 보세요. Xbox 호환(XInput) 패드만 직접 읽습니다. 그 밖의 패드는 Steam Input을 켜 주세요.
- 이슈를 올릴 때는 `log_level = 2`로 한 번 플레이한 뒤 `grail_input.log`를 첨부해 주세요.

## 동작 방식

Grail은 GameMaker YYC로 빌드되어 모든 GML 스크립트가 `grail.exe` 안의 네이티브 함수입니다. 이 모드는 `version.dll` 프록시로 게임 프로세스에 들어가서:

1. exe 안의 스크립트 이름표에서 `mouse_on`, `mouse_on_between`, `mouse_get_x/y`, `oGameManager` Step 이벤트를 찾아 [MinHook](https://github.com/TsudaKageyu/minhook)으로 후킹합니다.
2. 매 프레임 게임이 "마우스가 이 사각형 위에 있나?"를 물어보는 영역을 모읍니다. 이것이 버튼·카드·지도 칸 = 이동 대상입니다. 스크롤 영역·텍스트 상자처럼 올려도 반응이 없는 영역, 다른 대상을 감싸는 패널, 겹친 영역은 걸러냅니다.
3. 게임 좌표와 창 픽셀의 관계는 커서가 멈춰 있을 때의 `mouse_get_x/y` 값으로 자동 보정합니다.
4. 입력 스레드가 XInput 패드와 키보드를 읽어 커서 이동(`SetCursorPos`)과 클릭·휠·Esc(`SendInput`)를 만듭니다. 포인터 이동이 무시되는 환경(일부 gamescope)을 위해 게임의 `GetCursorPos`에는 가상 커서 위치를 알려줍니다.

### 직접 빌드

```
sudo apt install gcc-mingw-w64-x86-64 git zip   # Debian/Ubuntu
make dist                                       # -> build/grail-input-<version>.zip
```

`main` 브랜치에 push하면 GitHub Actions가 빌드하고, 새 버전이면 릴리스를 만듭니다.

---

## English

Adds **gamepad and keyboard controls** to Grail by Sokpop, on Windows and **Steam Deck (Proton)**. Unofficial fan mod, not affiliated with Sokpop.

**Install (Windows):** copy `version.dll` and `grail_input.ini` next to `grail.exe` (Steam → Manage → Browse local files).
**Install (Steam Deck):** same, then set the launch option `WINEDLLOVERRIDES="version=n,b" %command%` and pick the *Gamepad* controller layout.
**Uninstall:** delete `version.dll`.

**Controls:** left stick moves the cursor · D-pad / arrow keys jump to the nearest button or card in that direction · A / Enter / Space click (hold to drag) · B / Right Ctrl right-click · X / Start Esc · LB, RB, right stick / PageUp, PageDown scroll · LT slow, RT fast · Back / Home centre the cursor. Everything is configurable in `grail_input.ini`.

**How it works:** the game is a GameMaker YYC build; the mod hooks the game's own `mouse_on` hover tests to learn where every button and card is each frame, calibrates game coordinates against window pixels automatically, and drives the mouse with `SetCursorPos` / `SendInput` (plus a `GetCursorPos` override for environments that ignore pointer warps).

**Build:** `make dist` with `x86_64-w64-mingw32-gcc`.

## License

MIT (see `LICENSE`). Uses MinHook, BSD-2-Clause (see `THIRD_PARTY_NOTICES.md`).
