# Changelog

## v1.0.0-beta.4

- On-screen overlay (D3D11, also on Steam Deck / Proton):
  - a frame around the button or card the D-pad / arrow keys selected
  - the shortcut key next to every button that has a shortcut
  - short messages when a shortcut is set or cannot be used
- Shortcuts: 8 slots (F1-F8, slot 1 = LT, slot 2 = RT) that press a game button you choose.
  Bind in game with Ctrl + F1..F8, or hold BACK and press a pad button, while the cursor is on the button.
  Saved to `grail_input_shortcuts.ini`. Buttons drawn by shared code (the card piles, `button_draw`) are told apart by who drew them and where.
- LT / RT no longer slow down / speed up the cursor (free for shortcuts); BACK centres the cursor on a short tap

화면 위 표시(선택한 버튼 테두리, 단축키가 있는 버튼 옆 키 표시)와 단축키 기능을 추가했습니다. 버튼에 커서를 올리고 Ctrl+F1~F8(또는 BACK을 누른 채 패드 버튼)으로 지정하면 그 키가 그 버튼을 눌러 줍니다. LT/RT는 단축키 1·2번 칸이 되었습니다.

## v1.0.0-beta.3

(v1.0.0-beta.2 was released from an incomplete commit and only contains the beta.1 behaviour; use beta.3.)

- D-pad / arrow keys no longer jump to spots that do not react when hovered:
  - hover tests from scroll areas, text boxes, the battle log and cosmetic clipping are ignored (`[navigation] ignore_scripts`)
  - panels that only hold other targets (hand area, list backgrounds) are skipped (`[navigation] ignore_containers`)
  - the same spot tested twice counts once; targets mostly off screen are skipped
- Calibration takes three points, waits until the game has settled on each, and rejects inconsistent results (fixes a bad first calibration right after the window resizes at startup)
- `log_level = 2` logs which game script each target comes from

D패드·방향키가 마우스를 올려도 반응 없는 곳(스크롤 영역, 텍스트 상자, 전투 로그, 손패 영역 같은 패널, 겹친 영역)으로 가지 않게 했습니다. 시작 직후 좌표 보정이 틀어지던 문제도 고쳤습니다.

## v1.0.0-beta.1

First test build.

- Gamepad (XInput) and keyboard drive the mouse cursor, clicks, drag, scroll and Esc
- D-pad / arrow keys jump between the buttons, cards and map nodes the game is testing the mouse against
- Automatic calibration between game coordinates and window pixels
- Cursor position override for Proton / gamescope setups that ignore pointer warps
- Includes the Parsec cursor fix (`[cursor] parsec_fix`, off by default)
- Steam Deck: needs the launch option `WINEDLLOVERRIDES="version=n,b" %command%`

게임패드·키보드 조작, D패드/방향키 버튼·카드 점프, 스팀덱(Proton) 지원, Parsec 커서 수정 포함. 첫 테스트 빌드입니다.
