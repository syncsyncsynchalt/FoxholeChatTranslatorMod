## UE4リバースエンジニアリング知見 (Foxhole - UE4 4.24.3 Shipping)

### UE4オフセット (Foxhole UE4 4.24.3 x64, Dumper-7 SDK確認済み)
- ProcessEvent vtableインデックス: **66 (0x42)**
- ProcessEvent シグネチャ: `void __fastcall(void* thisObj, void* function, void* parms)`
- ProcessEvent関数アドレス: module+0x0150C3F0
- UObject::vtable: +0x00, ObjectFlags: +0x08, InternalIndex: +0x0C
- UObject::ClassPrivate: +0x10
- UObject::NamePrivate: +0x18 (FName = ComparisonIndex:int32 + Number:int32)
- UObject::OuterPrivate: +0x20
- UFunction::FunctionFlags: +0x98 (FUNC_Net=0x40, NetServer=0x200000, NetClient=0x1000000)
- UFunction::ExecFunction: +0x00C0, Total size: 0x00C8
- APlayerState::PlayerNamePrivate: +0x328
- GNames: module+0x04925900, GObjects: module+0x0493EC78

### UFunction / UStruct プロパティチェーン (parms解析用)
- UFunctionはUStructを継承。パラメータはFPropertyのリンクリストとして格納
- UStruct::ChildProperties → 最初のFProperty* (オフセットはランタイム検出が必要)
- FField(FPropertyの基底)レイアウト:
  - +0x00: VFT, +0x08: Class (FFieldClass*), +0x10: Owner (UObject*)
  - +0x20: Next (FField* → リンクリスト), +0x28: Name (FName)
- FPropertyの重要フィールド (オフセットはランタイム検出):
  - PropertyFlags: CPF_Parm=0x80, CPF_OutParm=0x100, CPF_ReturnParm=0x400
  - Offset_Internal: parmsバッファ内のオフセット (最重要)
  - ElementSize: 要素サイズ
- parms解析手順: ChildProperties→PropertyFlags&CPF_Parm→Offset_Internal→ElementSize→Next(+0x20)
- ChildProperties/PropertyFlags/Offset_Internal/ElementSizeはランタイム検出推奨。FField::Next=+0x20は全UE4バージョン固定

### UFunction判別
- function->ClassPrivate->NamePrivate を FName解決 → "Function" or "DelegateFunction" ならUFunction
- function->vtablePtr がモジュール内(.rdata)を指すかで検証可能

### GObjects配列
- FUObjectItem: Object(ptr+0x00), Flags(int32+0x08), SerialNumber(int32+0x0C) = 0x10バイト/要素
- チャンク配列: GObjects[ChunkIdx][ElementIdx * 0x10], 0x10000要素/チャンク

### GNames / FNamePool
- FNameEntryHeader: `(Len<<1) | bIsWide` → headerShift = **1** (UE4 4.24系)
- FNameBlockOffsetBits = **16** (ハードコード、OnProcessEvent初回100回でTryDetectShiftにより自動検証・補正)
- ComparisonIndex: `(block << 16) | (offset / stride)`, stride=2
- Block[0]検出: ヒープメモリ全域走査で "None" + gap(2/8) + "ByteProperty" の複合パターン
- FNamePool検出: Block[0]ポインタをモジュール内(.data)で逆引き → Blocks[]配列

### チャットparms構造体 (Dumper-7 SDK: War_parameters.hpp)
- ClientChatMessage (0x28): Channel+0x00, SenderPlayerState+0x08, MsgString+0x10
- ClientChatMessageWithTag (0x38): Channel+0x00, SenderPlayerState+0x08, RegTag+0x10, MsgString+0x20
- ClientWorldChatMessage (0x48): Message+0x00, SenderName+0x10, RegTag+0x20, Channel+0x41
- ServerChat (0x18): 送信RPCのため監視不要。除外済み（不正parmsからゴミデータが出力される）

### FNameEntryHeader 補足
- "None"のヘッダー: 0x0008 (len=4, shift=1)

### FNamePool Block[0] 検出の補足
- Block[0]はモジュール外のヒープ領域にある（モジュール内ではない）
- Blocks[]の直前8バイト: CurrentBlock(int32) + CurrentByteCursor(int32)

### FNamePool逆引き (名前→CI)
- FindFNameIndex(): FNamePoolの全ブロックを線形走査してCI取得
- チャット関連FName CI は起動ごとに変わらない

### parms解析手順の詳細
- parmsバッファ: フラット構造。各パラメータがOffset_Internalの位置に配置
- 解析手順:
  1. UFunction->ChildProperties で最初のFProperty取得
  2. FProperty->PropertyFlags & CPF_Parm (0x80) でパラメータか判定
  3. FProperty->Offset_Internal でparmsバッファ内の位置を取得
  4. FProperty->ElementSize で読み取りサイズを取得
  5. FProperty->Next (+0x20) で次のプロパティへ
  6. CPF_ReturnParm (0x400) は戻り値なのでスキップ
- FPropertyの追加フィールド: ArrayDim (配列次元)

### GObjects走査
- 起動時にGObjectsを走査してUFunctionを見つけ、プロパティオフセットをキャッシュ可能

### ゲーム更新時の自動対応
- TryDetectShift / ResolveFNameWithShift: FNameBlockOffsetBits自動検出

## 学んだ教訓

### SDK出力を最初に確認すること
- HWBPやランタイムデバッグより、Dumper-7等のSDK出力が信頼性高い
- HWBPでvtable[77]と測定 → 実際はDumper-7 SDKの通りvtable[66]だった
- 間違ったインデックスの症状: コールバックは大量に来るがプロパティ名しか見えない

### ServerChat (送信RPC) を監視してはいけない
- ServerChatはクライアント→サーバーの送信RPC (NetServer)
- UE4のRPC処理中に同じFName CIを持つ別のProcessEvent呼び出しが発生し、不正parmsからゴミデータが読まれる
- プレイヤー自身のメッセージはサーバーからClientChatMessage/WithTagとして折り返されるので機能的に不要

### FNameBlockOffsetBits自動検出の順序
- 降順 (16→14) で試行すること。昇順だとblock=0のCIがどのshiftでも同じ結果になり小さいshiftを誤検出
- CI >= 65536 のフィルタが必須 (block>0の場合のみ有効な判別可能)

### UE4 RPC関数の重複
- ClientChatMessage + ClientChatMessageWithTag が同一メッセージで両方発火する
- 500ms重複排除ウィンドウ (channel + sender + message をキーに) で対処

### 偽陽性UPropertyマッチへの対処
- FName CIの偶然一致でUPropertyなどがUFunctionと誤認される
- UFunction::FunctionFlags (offset 0x98) の FUNC_Net (0x40) 検証で偽陽性を除去

### ゲーム起動中のversion.dllコピー失敗は正常
- ゲームがversion.dllをロック中のためコピーが失敗するが、chat_translator.dllは正常にコピー・リロード可能

## Stage 1 試行錯誤の全記録

### 根本原因: vtableインデックスの誤り (最大の失敗)
- HWBPで77と測定 → 実際は66 (0x42)。Dumper-7 SDK `ProcessEventIdx = 0x00000042` で確定
- ChatGPTが最初に66を提案していたが、HWBPの実測結果を信じて77に変更してしまった

### 試行錯誤履歴
1. パターンスキャン + 単一ProcessEventフック → チャット不検出 (ProcessEventは仮想関数、複数オーバーライド)
2. マルチPEフック (vtable[77] × 32個) → コールバックは来るが全てチャット以外 (インデックス誤り)
3. PE hook数を32→64に拡張 → 変わらず (同上)
4. vtableスキャナー境界検出なし → 同一vtableのスライドで偽陽性大量発生 → 修正: addr[-1]境界検出
5. FNameBlockOffsetBits自動検出 (昇順14→16) → shift=14を誤検出 → 修正: 降順 + CI>=65536フィルタ
6. バースト名前検索 (2000万回) → チャット関数名なし (vtable[77]は別の仮想関数)
7. Dumper-7 SDK Basic.hpp確認 → `ProcessEventIdx = 0x42 = 66` 発見 → **根本原因解明**
8. vtable[66]に修正 → 即座にチャットキャプチャ成功
9. メッセージ重複 → 500ms重複排除ウィンドウ
10. 偽陽性UPropertyマッチ → FUNC_Net (0x40) 検証

### Stage 1 試行錯誤 詳細 (症状記録)

- vtable[77] (誤り) でのfunction引数の名前例: `ReceiveDrawHUD`, `LandscapeHeightfieldCollisionComponent`
- 間違ったインデックスの症状: プロパティ名(Vector, ArrayProperty等)しか見えない。UFunction名(ReceiveTick, ReceiveDrawHUD等)が出ないなら、フックしている関数がProcessEventではない
- vtableスキャナー境界検出なし時: 64個の"ユニーク"アドレスが実は同一大vtableのスライディングウィンドウ (vtable[N], vtable[N+1]... を別vtableのvtable[77]と誤認)
- バースト名前検索: 全PE呼び出しで名前解決 (2000万回) → チャット関数名なし (vtable[77]は別の仮想関数)
- ChatGPTが最初に66を提案していたが、HWBPの実測結果を信じて77に変更してしまった
