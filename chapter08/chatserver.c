/* socket/chatserver.c */
#include <arpa/inet.h>  /* htons(), htonl() を使うためのヘッダ */
#include <stdio.h>      /* fprintf(), perror() を使うためのヘッダ */
#include <stdlib.h>     /* exit() を使うためのヘッダ */
#include <string.h>     /* memset(), strlen() を使うためのヘッダ */
#include <sys/select.h> /* select(), FD_ZERO(), FD_SET(), FD_ISSET() を使うためのヘッダ */
#include <sys/types.h>  /* accept(), bind(), read(), setsockopt(), socket(), write() に関係する型定義 */
#include <sys/socket.h> /* socket(), setsockopt(), bind(), listen(), accept(), shutdown() を使うためのヘッダ */
#include <sys/uio.h>    /* read(), write() の宣言が入ることがある */
#include <unistd.h>     /* close(), read(), write() を使うためのヘッダ */

enum {
    /*
     * サーバが待ち受ける TCP ポート番号。
     *
     * クライアントはこのポートへ connect() してくる。
     * 今回はローカル実験用の 12345 番を使っている。
     */
    SERVER_PORT = 12345,

    /*
     * listen() に渡す backlog の目安。
     *
     * accept() される前の接続要求を、カーネルがどれくらい待ち行列に積めるかの
     * 目安として使われる。
     */
    NQUEUESIZE = 5,

    /*
     * このチャットサーバが同時に受け入れる最大クライアント数。
     *
     * この数を超える新規接続要求は sorry() で断る。
     */
    MAXNCLIENTS = 10,           /* 最大クライアント数 */
};

/*
 * clients[]:
 *   現在接続しているクライアントの「接続済みソケット fd」の一覧である。
 *
 *   listen 用ソケットとは別に、
 *   accept() が返したクライアントごとの fd をここへ保存する。
 *
 * nclients:
 *   現在 clients[] に有効に入っているクライアント数。
 *
 * つまり、
 *   clients[0] ～ clients[nclients-1]
 * が有効な接続先一覧である。
 *
 * このサーバの全体方針は、
 *
 *   1. 新しく accept() した fd を clients[] に追加する
 *   2. select() でその一覧を監視する
 *   3. あるクライアントから1行届いたら、それを全クライアントへ配る
 *   4. 切断されたら一覧から取り除く
 *
 * というものである。
 */
int clients[MAXNCLIENTS];       /* クライアントに繋がっている記述子の一覧 */
int nclients = 0;               /* 現在接続しているクライアント数 */

/*
 * sorry(ws)
 *
 * 接続してきたクライアントがいたが、すでに満員だった場合に
 * 「満席です」というメッセージを送る関数である。
 *
 * 引数:
 *   ws
 *     accept() で得た「今回の新規クライアント用の接続済みソケット fd」
 *
 * この関数は「メッセージ送信だけ」を担当し、
 * 実際の shutdown()/close() は呼び出し元で行っている。
 */
void sorry(int ws) {
    /*
     * 接続拒否時に返す固定文。
     *
     * 英語としては
     *   "Sorry, it's full."
     * で、
     * 「申し訳ないが満員です」という意味になる。
     */
    char *message = "Sorry, it's full.\n";

    /*
     * write(ws, message, strlen(message))
     *
     * 接続済みソケット ws に対してこの固定メッセージを送る。
     *
     * ソケットも file descriptor の一種なので、
     * 通信中であれば通常ファイルと同様に write() できる。
     *
     * 戻り値チェックは省略しているが、
     * 本来は -1 の確認や部分書き込みの考慮があってもよい。
     */
    write(ws, message, strlen(message));
}

/*
 * delete_client(ws)
 *
 * 接続済みクライアント一覧 clients[] から、
 * descriptor ws を取り除く関数である。
 *
 * このサーバでは clients[] を「詰めた配列」として管理しているため、
 * 真ん中を消すときは単に空きを作るのではなく、
 * 末尾要素をそこへ移して nclients を 1 減らしている。
 *
 * つまり O(1) に近い簡単な削除法を採用している。
 *
 * 削除前:
 *   clients = [3, 5, 8, 10], nclients = 4
 *
 * たとえば ws = 5 を削除すると、
 *
 * 削除後:
 *   clients = [3, 10, 8, ...], nclients = 3
 *
 * となる。
 *
 * 順序は保存しないが、
 * 「今どの fd が有効か」だけ管理できれば十分なので問題ない。
 */
void delete_client(int ws) {
    int i;

    for (i = 0; i < nclients; i++) {
        /*
         * 消したい記述子 ws を見つけたら、
         * 最後尾の要素をここへ持ってきて、
         * 要素数を 1 減らす。
         *
         * この方法の利点:
         *   - 後ろをずらすループが要らない
         *   - 高速で簡単
         *
         * 欠点:
         *   - clients[] の並び順は保たれない
         *
         * ただしこのコードでは並び順に意味は無いので問題ない。
         */
        if (clients[i] == ws) {
            clients[i] = clients[nclients - 1];
            nclients--;
            break;
        }
    }
}

/*
 * talks(ws)
 *
 * あるクライアント ws から「1 行」を読み取り、
 * それを現在接続中の全クライアントへ 1 文字ずつ配布する関数である。
 *
 * ここで重要なのは「1行」という単位で処理している点である。
 * この関数は改行 '\n' が来るまで 1 文字ずつ read() し、
 * 読めた文字を全クライアントへその場で write() していく。
 *
 * つまり入力の意味論としては、
 *   「改行で 1 メッセージ終端」
 * という行指向チャットになっている。
 *
 * 引数:
 *   ws
 *     発言元クライアントの接続済みソケット fd
 *
 * 注意:
 *   この関数はクライアント切断時に
 *     delete_client(ws)
 *   を呼ぶため、
 *   clients[] と nclients の中身を変更する。
 *
 *   そのため呼び出し側では、
 *   この関数を呼んだあとに clients[] をそのまま続けて走査すると危険であり、
 *   実際に main() 側では break して再度 select() ループに戻っている。
 */
void talks(int ws) {
    int i, cc;
    char c;

    /*
     * do-while で回している理由:
     *   最低でも 1 文字は読む必要があり、
     *   その後「その文字が改行かどうか」で継続判定したいからである。
     *
     * 全体のアルゴリズムはこうである。
     *
     *   1. ws から 1 文字読む
     *   2. EOF なら接続終了処理をして return
     *   3. 読んだ文字を全クライアントに配る
     *   4. その文字が '\n' なら 1 行終わりなので関数終了
     *   5. そうでなければ次の 1 文字へ
     */
    do {
        /*
         * read(ws, &c, 1)
         *
         * 発言元クライアント ws から 1 バイト読む。
         *
         * ここで 1 バイトずつ読んでいるのは、
         * 改行 '\n' をメッセージ境界とみなす単純な行指向プロトコルを
         * 実装したいからである。
         *
         * read() の戻り値 cc の意味:
         *   cc == -1 : エラー
         *   cc ==  0 : EOF（クライアントが接続を閉じた）
         *   cc ==  1 : 1 文字読めた
         */
        if ((cc = read(ws, &c, 1)) == -1) {
            /*
             * 読み取りエラー。
             *
             * 簡潔なサンプルなので即座にサーバ全体を異常終了している。
             * 実運用ならクライアント単位のエラーとして扱う設計もありうる。
             */
            perror("read");
            exit(1);
        } else if (cc == 0) {         /* EOF; クライアントが通信路を切った */
            /*
             * cc == 0 は EOF であり、
             * クライアント側が接続を閉じたことを意味する。
             *
             * この場合の処理手順:
             *   1. shutdown() で通信停止
             *   2. close() で fd 解放
             *   3. clients[] から削除
             *   4. ログ表示
             *   5. return
             *
             * shutdown(ws, SHUT_RDWR):
             *   送受信の両方向を停止する。
             *
             * close(ws):
             *   プロセス側の fd 資源を解放する。
             *
             * delete_client(ws):
             *   接続一覧からこのクライアントを外す。
             *
             * ここで return することで、
             * 「もうこのクライアントからの 1 行処理は終わった」
             * として呼び出し元へ戻る。
             */
            shutdown(ws, SHUT_RDWR);
            close(ws);
            delete_client(ws);
            fprintf(stderr, "Connection closed on descriptor %d.\n", ws);
            return;
        }

        /*
         * 読んだ 1 文字 c を、現在接続している全クライアントへ送る。
         *
         * ここでは発言者本人も含めて全員に送っている。
         * つまりエコーバックも兼ねる形である。
         *
         * アルゴリズム的には「ブロードキャスト」である。
         *
         *   for 各クライアント fd in clients[]
         *       write(fd, &c, 1)
         *
         * としているので、
         * 発言元クライアント以外へだけ送る設計ではないことに注意する。
         */
        for (i = 0; i < nclients; i++)
            write(clients[i], &c, 1);
    } while (c != '\n');

    /*
     * c == '\n' になったら 1 行ぶんの中継が終わったので関数終了。
     *
     * つまりこの関数は、
     *   「1 クライアントから 1 行届いたら、その 1 行を全体へ配る」
     * という単位で動いている。
     */
}

int main(void) {
    /*
     * s:
     *   待ち受け用ソケット（listening socket）。
     *
     * soval:
     *   SO_REUSEADDR の設定値。
     *
     * sa:
     *   サーバ自身のローカルアドレス情報。
     */
    int s, soval;
    struct sockaddr_in sa;

    /*
     * ------------------------------------------------------------
     * 1. socket() で待ち受け用ソケットを作る
     * ------------------------------------------------------------
     *
     * socket(AF_INET, SOCK_STREAM, 0)
     *
     * 意味:
     *   IPv4 / TCP 用のソケットを作る。
     *
     * まだこの時点では
     *   - どのポートで待つか
     *   - どのアドレスで待つか
     *   - サーバとして待受状態にするか
     * は決まっていない。
     */
    if ((s = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        exit(1);
    }

    /*
     * ------------------------------------------------------------
     * 2. setsockopt() で SO_REUSEADDR を有効にする
     * ------------------------------------------------------------
     *
     * サーバ再起動時に bind() しやすくするための定番設定。
     */
    soval = 1;
    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &soval, sizeof(soval)) == -1) {
        perror("setsockopt");
        exit(1);
    }

    /*
     * ------------------------------------------------------------
     * 3. bind() 用の sockaddr_in を作る
     * ------------------------------------------------------------
     *
     * sa には
     *   - IPv4
     *   - ポート 12345
     *   - INADDR_ANY
     * を設定する。
     *
     * INADDR_ANY を使うことで、
     * このホスト上のどの IPv4 宛ての接続でも受けられる。
     */
    memset(&sa, 0, sizeof(sa));

    /*
     * BSD 系には sin_len がある場合があるが、
     * Linux の struct sockaddr_in には通常存在しない。
     *
     * Linux ではこの行はコンパイルエラーになるので削除または無効化が必要。
     */
    /* sa.sin_len = sizeof(sa); */     /* sin_len がある場合。Linux では通常不要 */

    sa.sin_family = AF_INET;
    sa.sin_port = htons(SERVER_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);

    /*
     * bind()
     *
     * このソケットをローカルポート 12345 に結び付ける。
     * サーバではここで初めて「どのポートで待つか」が決まる。
     */
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) == -1) {
        perror("bind");
        exit(1);
    }

    /*
     * listen()
     *
     * 待ち受けソケットとして有効化する。
     *
     * これにより s は accept() の対象となる「受付窓口」になる。
     */
    if (listen(s, NQUEUESIZE)) {
        perror("listen");
        exit(1);
    }

    fprintf(stderr, "Ready.\n");

    /*
     * ------------------------------------------------------------
     * 4. select() ベースのイベントループ
     * ------------------------------------------------------------
     *
     * このサーバは fork もスレッドも使わず、
     * 1 プロセスの中で複数のクライアントを扱う。
     *
     * そのために使っているのが select() である。
     *
     * select() の考え方:
     *   「複数の fd のうち、今 read 可能になったものがあるか待つ」
     *
     * つまりこのサーバは
     *
     *   - 新しい接続要求が来たか
     *   - 既存クライアントの誰かが発言したか
     *
     * を 1 本のイベントループで監視している。
     */
    for (;;) {
        int i, maxfd;
        fd_set readfds;

        /*
         * maxfd:
         *   select() の第1引数に渡す最大 fd 値 + 1 を計算するための変数。
         *
         * readfds:
         *   「読み取り可能になるのを監視したい fd の集合」。
         *
         * select() はこの fd_set を見て、
         * どの fd が ready になったかを返してくる。
         */

        /*
         * --------------------------------------------------------
         * 4-1. select() 用の監視集合を作る
         * --------------------------------------------------------
         */

        /*
         * FD_ZERO(&readfds)
         *
         * fd 集合を空集合に初期化する。
         *
         * コメントの「御破算で願いましては」はそろばん風の言い回しで、
         * 一旦全部クリアして最初から作り直す、という意味である。
         */
        FD_ZERO(&readfds);      /* 御破算で願いましては */

        /*
         * 待ち受けソケット s を監視集合へ入れる。
         *
         * これにより、
         *   「新しい接続要求が来て accept() 可能になったか」
         * を select() で検知できる。
         */
        FD_SET(s, &readfds);    /* 接続待ちソケットを見張る */

        /*
         * maxfd の初期値はまず s にしておく。
         * これから clients[] も見ながら最大値を更新する。
         */
        maxfd = s;

        /*
         * 既存クライアント全員の fd も監視集合へ入れる。
         *
         * これにより、
         *   「誰かが発言して read 可能になったか」
         * を select() で検知できる。
         *
         * 同時に select() のために最大 fd 値も計算している。
         */
        for (i = 0; i < nclients; i++) {
            FD_SET(clients[i], &readfds);
            if (clients[i] > maxfd)
                maxfd = clients[i];
        }

        /*
         * --------------------------------------------------------
         * 4-2. select() で「何か起きるまで待つ」
         * --------------------------------------------------------
         *
         * select(maxfd+1, &readfds, NULL, NULL, NULL)
         *
         * 第1引数:
         *   監視対象 fd の最大値 + 1
         *
         * 第2引数:
         *   読み取り可能になるのを待つ fd 集合
         *
         * 第3引数:
         *   書き込み可能待ち集合（今回は使わない）
         *
         * 第4引数:
         *   例外待ち集合（今回は使わない）
         *
         * 第5引数:
         *   タイムアウト（NULL なので無期限待機）
         *
         * 戻り値:
         *   >0 : 準備できた fd の数
         *    0 : タイムアウト（今回は起きない）
         *   <0 : エラー
         *
         * 重要:
         *   select() から戻ると readfds は「ready だった fd だけが残る集合」に
         *   書き換えられる。
         *
         *   だからその後は FD_ISSET() で
         *   「どの fd が ready か」を調べられる。
         */
        if (select(maxfd+1, &readfds, NULL, NULL, NULL) < 0) {
            perror("select");
            exit(1);
        }

        /*
         * --------------------------------------------------------
         * 4-3. 新しい接続要求が来たか判定する
         * --------------------------------------------------------
         *
         * 待ち受けソケット s が ready なら、
         * accept() できる新規接続が来ているという意味である。
         */
        if (FD_ISSET(s, &readfds)) {
            /*
             * ca:
             *   接続してきたクライアントのアドレス情報を受け取る構造体。
             *
             * ca_len:
             *   その構造体のサイズ。
             *
             * ws:
             *   accept() が返す、今回のクライアントとの接続済みソケット。
             */
            struct sockaddr_in ca;
            socklen_t ca_len;
            int ws;

            /*
             * accept() 呼び出し前に ca のサイズを設定しておく。
             */
            ca_len = sizeof(ca);

            /*
             * accept()
             *
             * 待ち受けソケット s に届いている接続要求を 1 件受理し、
             * その接続専用のソケット ws を返す。
             *
             * 重要:
             *   s は残る。
             *   実際の会話は ws で行う。
             */
            if ((ws = accept(s, (struct sockaddr *)&ca, &ca_len)) == -1) {
                perror("accept");
                continue;
            }

            /*
             * もしすでに満員なら、
             *   1. Sorry メッセージを送る
             *   2. shutdown()
             *   3. close()
             *   4. ログ表示
             * して終わる。
             *
             * つまり「受理はしたが、参加はさせない」という扱いである。
             */
            if (nclients >= MAXNCLIENTS) {
                /* もう満杯 */
                sorry(ws);
                shutdown(ws, SHUT_RDWR);
                close(ws);
                fprintf(stderr, "Refused a new connection.\n");
            } else {
                /*
                 * まだ空きがあれば clients[] に追加する。
                 *
                 * これで次回以降の select() 監視対象にも入る。
                 */
                clients[nclients] = ws; /* 記述子一覧に加える */
                nclients++;
                fprintf(stderr, "Accepted a connection on descriptor %d.\n", ws);
            }
        }

        /*
         * --------------------------------------------------------
         * 4-4. 既存クライアントの誰かが発言したか判定する
         * --------------------------------------------------------
         *
         * ここでは clients[] を順に見て、
         * FD_ISSET(clients[i], &readfds) が真なら
         * そのクライアントが read 可能、つまり何か発言したとみなす。
         *
         * その場合 talks() を呼んで
         * 1 行ぶんを全体へ中継する。
         */
        for (i = 0; i < nclients; i++) {
            if (FD_ISSET(clients[i], &readfds)) {
                /*
                 * talks() は以下を行う可能性がある。
                 *
                 *   - read() で文字を読む
                 *   - broadcast する
                 *   - クライアントが切断していれば delete_client() で
                 *     clients[] と nclients を変更する
                 *
                 * そのため、この for ループをそのまま続行すると
                 * 配列内容が変わった状態で走査を続けることになり危険である。
                 *
                 * そこで 1 件処理したら break して、
                 * 次のループ先頭へ戻り、新しく fd_set を作り直して select() し直す。
                 */
                talks(clients[i]);
                break;  /* talksはclients[]の中身とnclientsを変えることがある */
            }
        }

        /*
         * ここまで終わったらループ先頭へ戻る。
         *
         * つまりこのサーバは毎回
         *
         *   1. 監視集合を作り直す
         *   2. select() で待つ
         *   3. 新規接続か既存クライアント発言かを処理する
         *   4. また最初へ戻る
         *
         * というイベント駆動ループで動いている。
         */
        /* 繰り返しの先頭に戻って再びselectする */
    }
}
