grail-input @VERSION@
Grail(Sokpop)에 게임패드·키보드 조작을 더하는 비공식 모드
Gamepad and keyboard controls for Grail by Sokpop (unofficial fan mod)

==================== 설치 (Windows) ====================
1. Steam 라이브러리에서 Grail 우클릭 -> 관리 -> 로컬 파일 보기
2. 이 zip의 version.dll, grail_input.ini 를 grail.exe 가 있는 폴더에 복사
3. 게임 실행. 같은 폴더에 grail_input.log 가 생기면 적용된 것

==================== 설치 (스팀덱) ====================
1. 데스크톱 모드에서 Grail -> 관리 -> 로컬 파일 보기
2. version.dll, grail_input.ini 를 grail.exe 옆에 복사
3. Grail 속성 -> 일반 -> 실행 옵션에 입력:
     WINEDLLOVERRIDES="version=n,b" %command%
4. 컨트롤러 설정에서 레이아웃을 "게임패드"로 선택

==================== 제거 ====================
version.dll 을 지우면 원래대로 돌아갑니다. (스팀덱은 실행 옵션도 지우기)

==================== 기본 조작 ====================
왼쪽 스틱 / Shift+방향키      커서 이동
D패드 / 방향키                그 방향의 가장 가까운 버튼·카드로 점프
A / Enter, Space              클릭 (누르고 있으면 드래그)
B / 오른쪽 Ctrl               오른쪽 클릭
X, START                      Esc (메뉴·닫기)
LB, RB, 오른쪽 스틱 / PgUp, PgDn   스크롤
LT / RT, F1 ~ F8              단축키 (아래 참고)
BACK 짧게 / Home              커서를 화면 가운데로

==================== 단축키 ====================
게임 버튼에 커서를 올리고 Ctrl + F1 ~ F8 (패드: BACK을 누른 채 LT 등)
-> 그 키가 그 버튼을 눌러 줍니다. 버튼 옆에 키가 표시됩니다.
1번 칸 = F1 / LT, 2번 칸 = F2 / RT. 빈 곳에서 하면 비워집니다.

버튼·키는 grail_input.ini 에서 바꿀 수 있습니다.
Parsec 원격 플레이: grail_input.ini 의 [cursor] parsec_fix = transparent

==================== English ====================
Install: copy version.dll and grail_input.ini next to grail.exe.
Steam Deck: also set the launch option  WINEDLLOVERRIDES="version=n,b" %command%
            and choose the "Gamepad" controller layout.
Uninstall: delete version.dll.
Controls: left stick = cursor, D-pad/arrows = jump between buttons and cards,
A/Enter/Space = click (hold to drag), B/Right Ctrl = right click, X/Start = Esc,
LB/RB/right stick/PageUp/PageDown = scroll, Back/Home = centre.
Shortcuts: cursor on a game button + Ctrl+F1..F8 (pad: hold Back + LT...) binds that key;
slot 1 = F1/LT, slot 2 = F2/RT.
Problems: set log_level = 2 in grail_input.ini and check grail_input.log.
