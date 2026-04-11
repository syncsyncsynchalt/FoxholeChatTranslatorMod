## UE4リバースエンジニアリング知見 (Foxhole - UE4 4.24.3 Shipping)

### ProcessEvent
- vtableインデックス: **66 (0x42)** — Dumper-7 SDK `ProcessEventIdx = 0x00000042` で確定
- ProcessEvent関数アドレス: module+0x0150C3F0
- シグネチャ: `void __fastcall(void* thisObj, void* function, void* parms)`
- **重要**: ProcessEventはvirtual関数。各UEクラスがオーバーライド可能 → 全実装をフックする必要がある

### マルチPEフック方式 (正解)
- .rdataセクション内の全vtableを走査し、vtable[66]のユニークなアドレスを収集
- **vtable先頭境界検出が必須**: addr[-1]がモジュール内コードを指さない位置 = vtable先頭
  - これがないとスライディングウィンドウで大量の偽陽性が出る
- vtable候補判定: [0],[1],[2]がモジュール内コードを指すか確認
- 各PEアドレスにMinHookで個別フック。DEFINE_PE_HOOK(N)マクロで0-63の個別detour関数を生成

### GNames / FNamePool (UE4 4.24.3)
- Foxholeは **UE4 4.24.3** (`4.24.3-0+++UE4+Release-4.24-War`)
- FNameEntryHeader形式: `(Len<<1) | bIsWide` → headerShift = **1** (4.24系)
  - "None"のヘッダー: 0x0008 (len=4, shift=1)
- **FNameBlockOffsetBits = 16** (自動検出で確認済み)
- ComparisonIndex: `(block << 16) | (offset / stride)`, stride=2
- GNames: module+0x04925900
- GObjects: module+0x0493EC78
- Block[0]検出: メモリ全域を走査し、"None"(4bytes) + gap(2or8bytes) + "ByteProperty"(12bytes) の複合パターン
  - モジュール外のヒープ領域にある（モジュール内ではない）
- FNamePool検出: Block[0]ポインタをモジュール内(.data)で逆引き → Blocks[]配列の先頭
  - Blocks[]の直前8バイト: CurrentBlock(int32) + CurrentByteCursor(int32)

### UObjectオフセット (UE4 4.25+ x64)
- vtable:       +0x00
- ObjectFlags:  +0x08
- InternalIndex:+0x0C
- ClassPrivate: +0x10
- NamePrivate:  +0x18 (FName = ComparisonIndex:int32 + Number:int32)
- OuterPrivate: +0x20

### UFunction / UStruct プロパティチェーン (★parms解析に必須)
- UFunctionはUStructを継承。パラメータはFPropertyのリンクリストとして格納
- **UStruct::ChildProperties** → 最初のFProperty* (オフセットはランタイム検出が必要)
- **FField(FPropertyの基底)レイアウト**:
  - +0x00: VFT
  - +0x08: Class (FFieldClass*)
  - +0x10: Owner (UObject*)
  - +0x20: Next (FField* → 次のプロパティへのリンクリスト)
  - +0x28: Name (FName)
- **FPropertyの重要フィールド** (オフセットはランタイム検出):
  - ArrayDim: 配列次元
  - ElementSize: 要素サイズ
  - PropertyFlags: EPropertyFlags (CPF_Parm=0x80, CPF_OutParm=0x100, CPF_ReturnParm=0x400)
  - Offset_Internal: **parmsバッファ内のオフセット** ← これが最重要
- **parmsバッファ**: フラット構造。各パラメータがOffset_Internalの位置に配置
- **parms解析手順**:
  1. UFunction->ChildProperties で最初のFProperty取得
  2. FProperty->PropertyFlags & CPF_Parm (0x80) でパラメータか判定
  3. FProperty->Offset_Internal でparmsバッファ内の位置を取得
  4. FProperty->ElementSize で読み取りサイズを取得
  5. FProperty->Next (+0x20) で次のプロパティへ
  6. CPF_ReturnParm (0x400) は戻り値なのでスキップ
- **重要**: ChildProperties/PropertyFlags/Offset_Internal/ElementSizeのオフセットは
  ハードコードせず、ランタイムでDumper-7方式のFindOffset()で検出すべき。
  ただし FField::Next=+0x20 は全UE4バージョンで固定

### UFunction フラグ (EFunctionFlags)
- FunctionFlags offset: **0x0098** (Dumper-7 SDK確認)
- ExecFunction (native func ptr) offset: **0x00C0**
- UFunction total size: 0x00C8
- **FUNC_Net = 0x40** — UFunction判別に使用（CI一致 + FUNC_Net検証で偽陽性除去）
- NetServer = 0x200000 (Server RPC: クライアント→サーバー)
- NetClient = 0x1000000 (Client RPC: サーバー→クライアント)
- チャット関連: ClientChatMessage=NetClient, ClientChatMessageWithTag=NetClient, ClientWorldChatMessage=NetClient
- ServerChat(NetServer)は送信RPCであり、監視対象から除外済み（不正parms読み取りでゴミデータが出力されるため）

### GObjects配列
- FUObjectItem: Object(ptr+0x00), Flags(int32+0x08), SerialNumber(int32+0x0C) = 0x10バイト/要素
- チャンク配列(4.25+): GObjects[ChunkIdx][ElementIdx * 0x10], 0x10000要素/チャンク
- 起動時にGObjectsを走査してUFunctionを見つけ、プロパティオフセットをキャッシュ可能

### UFunction判別
- function引数が本当にUFunctionか検証する方法:
  1. function->vtablePtr がモジュール内(.rdata)を指すか
  2. function->ClassPrivate->NamePrivate を FName解決 → "Function" を含むか
- ClassPrivate が "Function" or "DelegateFunction" ならUFunction

### FNamePool逆引き (名前→CI)
- FindFNameIndex(): FNamePoolの全ブロックを線形走査してCI取得
- チャット関連FName CI（起動ごとに変わらない）:
  - ClientChatMessage, ClientChatMessageWithTag, ClientWorldChatMessage
  - ServerChatは送信RPCであり監視不要（除外済み）

### RE-UE4SS 代替アプローチ
- RE-UE4SS (2415 stars): 汎用UE4/5 modフレームワーク。dwmapi.dllプロキシでロード
- RegisterHook("/Script/War.WarPlayerController:ClientChatMessage", callback) のようにLuaから特定UFunction をフック可能
- パラメータは自動デコード: コールバックに self + params が渡される
- Foxholeで動作するかは未確認だが、カスタムDLLが行き詰まった場合の有力な代替手段

### Dumper-7 SDK生成 (実行済み)
- Dumper-7 (1785 stars): UE4/5用SDKジェネレーター。DLLインジェクションで動作
- Foxhole SDK出力: `C:\Dumper-7\4.24.3-0+++UE4+Release-4.24-War\CppSDK\`
- UE4バージョン: **4.24.3** (-0+++UE4+Release-4.24-War)
- SDK生成手順: cmake build → CreateRemoteThread LoadLibraryA で注入
- **チャットparms構造体** (War_parameters.hpp):
  - ClientChatMessage (0x28): Channel+0x00, SenderPlayerState+0x08, MsgString+0x10
  - ClientChatMessageWithTag (0x38): +RegimentTag+0x10, MsgString+0x20
  - ClientWorldChatMessage (0x48): Message+0x00, SenderName+0x10, RegTag+0x20
  - ServerChat (0x18): Message+0x00, ChatChannel+0x10 — 送信RPCのため監視不要、実装から除外済み
- APlayerState::PlayerNamePrivate = +0x328 (FString)

### アーキテクチャ
- **version.dll** (永続): プロキシ17関数 + MinHookフック + ワーカーDLL管理
- **chat_translator.dll** (ホットリロード可能): GNames検出 + OnProcessEventコールバック
  - TryDetectShift / ResolveFNameWithShift: ゲーム更新時のFNameBlockOffsetBits自動検出
- ワーカーリロード: version.dllが2秒ごとにFILETIMEを比較、変更検知で自動リロード

### 犯した間違い・教訓 (Stage 1 全記録)

#### ★ 根本原因: vtableインデックスの誤り (最大の失敗)
- **HWBPで77と測定 → 実際は66 (0x42)**
- Dumper-7 SDK の `ProcessEventIdx = 0x00000042` で確定
- HWBPが77を返した原因: ブレークポイントが正しい関数にヒットしていなかった可能性
- **教訓**: ゲーム固有のオフセットは必ずDumper-7等のSDKジェネレーターの出力と照合する
- **皮肉**: ChatGPTが最初に66を提案していたが、HWBPの実測結果を信じて77に変更してしまった

#### 試行錯誤の全履歴
1. **初期アプローチ: パターンスキャン + 単一ProcessEventフック** → チャット不検出
   - 失敗理由: ProcessEventは仮想関数、複数オーバーライドが存在
2. **マルチPEフック (vtable[77] × 32個)** → コールバックは来るが全てチャット以外
   - 失敗理由: vtableインデックスが間違っていた (77は別の仮想関数)
   - function引数の名前: `ReceiveDrawHUD`, `LandscapeHeightfieldCollisionComponent` 等
3. **PE hook数を32→64に拡張** → 変わらず
   - 失敗理由: 同上 (インデックスが根本的に間違い)
4. **vtableスキャナー改良なし (境界検出なし)** → 64個の"ユニーク"アドレスが実は同一大vtableのスライド
   - 失敗理由: vtable[N], vtable[N+1]... を別vtableのvtable[77]と誤認
   - 修正: addr[-1]がモジュール外→vtable先頭と判定する境界検出を追加
5. **FNameBlockOffsetBits自動検出 (昇順14→16)** → shift=14を誤検出
   - 失敗理由: block=0のCIはどのshiftでも同じ結果 → 小さいshiftが先にマッチ
   - 修正: 降順16→14 + CI>=65536フィルタ (block>0が必須)
6. **バースト名前検索 (全PE呼び出しで名前解決、2000万回)** → チャット関数名なし
   - 失敗理由: vtable[77]は別の仮想関数なのでチャット関数は絶対に通らない
7. **Dumper-7 SDK Basic.hpp確認** → `ProcessEventIdx = 0x42 = 66` を発見 → **根本原因解明**
8. **vtable[66]に修正** → 即座にチャットキャプチャ成功

#### チャット検出成功後の追加修正
9. **メッセージ重複**: ClientChatMessage + ClientChatMessageWithTag が両方発火
   - 修正: 500ms以内の同一メッセージを重複排除
10. **偽陽性UPropertyマッチ**: CIの偶然一致でUPropertyをUFunctionと誤認
   - 修正: UFunction FunctionFlags (offset 0x98) の FUNC_Net (0x40) 検証

#### メタ教訓
- **SDK出力を最初に確認**: HWBPやランタイムデバッグより、Dumper-7等のSDK出力が信頼性高い
- **間違ったインデックスの症状**: コールバックは大量に来るが、プロパティ名(Vector, ArrayProperty等)しか見えない。UFunction名(ReceiveTick, ReceiveDrawHUD等)が出ないなら、フックしている関数がProcessEventではない
- **重複の原因診断**: UE4のRPC関数は、名前が似た複数バリアント(WithTag等)で同じイベントが2回呼ばれることがある
- **vtableスキャナーの偽陽性**: 境界検出なしだと、1つの大きなvtable内の連続エントリを別vtableと誤認する
