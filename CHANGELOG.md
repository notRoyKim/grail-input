# Changelog

## v1.0.0-beta.1

First test build.

- Gamepad (XInput) and keyboard drive the mouse cursor, clicks, drag, scroll and Esc
- D-pad / arrow keys jump between the buttons, cards and map nodes the game is testing the mouse against
- Automatic calibration between game coordinates and window pixels
- Cursor position override for Proton / gamescope setups that ignore pointer warps
- Includes the Parsec cursor fix (`[cursor] parsec_fix`, off by default)
- Steam Deck: needs the launch option `WINEDLLOVERRIDES="version=n,b" %command%`

게임패드·키보드 조작, D패드/방향키 버튼·카드 점프, 스팀덱(Proton) 지원, Parsec 커서 수정 포함. 첫 테스트 빌드입니다.
