/* socket/pindepclient.c */
#include <sys/types.h>  /* connect(), read(), socket(), write() などに関係する型定義 */
#include <stdio.h>      /* fprintf(), perror() を使うためのヘッダ */
#include <stdlib.h>     /* exit() を使うためのヘッダ */
#include <string.h>     /* memset(), strlen() を使うためのヘッダ */
#include <sys/socket.h> /* socket(), connect(), shutdown() を使うためのヘッダ */
#include <netdb.h>      /* getaddrinfo(), freeaddrinfo(), gai_strerror(), struct addrinfo */
#include <sys/uio.h>    /* read(), write() の宣言が入ることがある */
#include <unistd.h>     /* read(), write(), close() を使うためのヘッダ */

/*
 * このプログラムの主題は、
 *
 *   「getaddrinfo() を使って、プロトコルやアドレス族に依存しにくい形で
 *    ホスト名へ接続し、HTTP 要求を送って応答を受け取る」
 *
 * ことである。
 *
 * ファイル名の pindepclient は、
 *   protocol independent client
 * の略だと考えると理解しやすい。
 *
 * 昔ながらのコードでは
 *
 *   - IPv4 専用の sockaddr_in を自分で埋める
 *   - inet_addr() や gethostbyname() を使う
 *
 * といった形が多かった。
 *
 * しかしその方法だと、
 *   - IPv6 に対応しにくい
 *   - ホスト名解決とアドレス構造体生成が分離していて扱いにくい
 * といった問題がある。
 *
 * そこで現在の標準的な書き方では、
 *
 *   getaddrinfo()
 *
 * を使って
 *
 *   「このホスト名に対して、この種別の通信をしたい」
 *
 * という条件を与え、
 * その結果として使えるアドレス候補リストを受け取る。
 *
 * このコードでは具体的に、
 *
 *   1. "www.titech.ac.jp" の "http" サービスへ接続できる候補を
 *      getaddrinfo() で得る
 *   2. その先頭候補を使って socket() と connect() を行う
 *   3. HTTP/1.0 の GET 要求を送る
 *   4. サーバからの返答を read() で最後まで受け取り、
 *      そのまま標準出力へ流す
 *
 * という流れになっている。
 *
 * つまりこのプログラムは、
 *
 *   「名前解決 + 接続 + HTTP 要求送信 + 応答受信」
 *
 * を最小限でまとめたネットワーククライアントである。
 */

/*
 * 送信する HTTP 要求文字列。
 *
 *   GET / HTTP/1.0
 *
 * は「トップページ / を取得したい」という意味である。
 *
 * 末尾の \n\n は HTTP リクエストヘッダ終端を表したい意図だが、
 * 現在の HTTP では CRLF（\r\n）を使うのがより正確である。
 *
 * つまり厳密には
 *
 *   "GET / HTTP/1.0\r\n\r\n"
 *
 * の方がより標準的である。
 *
 * ただし単純な実験サーバではこの書き方でも通ることがある。
 *
 * このコードの本質は HTTP 厳密実装ではなく、
 * ソケット通信と getaddrinfo() の流れを学ぶことにある。
 */
char *httpreq = "GET / HTTP/1.0\n\n";   /* トップページを得るためのHTTP要求 */

int main(void) {
    /*
     * hints:
     *   getaddrinfo() に「どんな候補を欲しいか」を伝えるための条件構造体。
     *
     *   たとえば
     *     - IPv4 に限るか / IPv6 も許すか
     *     - TCP か UDP か
     *   といった条件をここへ入れる。
     *
     * addrs:
     *   getaddrinfo() が返す addrinfo 構造体の連結リストの先頭ポインタ。
     *
     *   このリストには
     *     - 使えるアドレス族
     *     - ソケット種別
     *     - プロトコル番号
     *     - 接続先 sockaddr
     *   などが並ぶ。
     *
     * cc:
     *   いくつかの関数の戻り値を受ける変数。
     *
     *   特に
     *     - getaddrinfo() の戻り値
     *     - read() の戻り値
     *   に使っている。
     *
     * s:
     *   socket() が返すソケット fd。
     *
     *   connect(), write(), read(), shutdown(), close() の対象になる。
     *
     * buf:
     *   サーバから read() で受信したデータを一時的に格納するバッファ。
     */
    struct addrinfo hints, *addrs;
    int cc, s; 
    char buf[1024];

    /*
     * ------------------------------------------------------------
     * 1. getaddrinfo() に渡す条件 hints を作る
     * ------------------------------------------------------------
     *
     * getaddrinfo() は
     *   「ホスト名 + サービス名」から、接続候補の addrinfo リストを返す
     * 関数である。
     *
     * その際、hints に
     *   「どんな候補が欲しいか」
     * を書いておく。
     */

    /*
     * まず hints 全体を 0 で初期化する。
     *
     * 理由:
     *   struct addrinfo には複数のフィールドがあり、
     *   明示的に使うもの以外は 0 にしておくのが安全だからである。
     *
     * ネットワーク系の構造体では、
     * 「まず memset でゼロクリアして、必要な項目だけ設定する」
     * のが基本である。
     */
    memset(&hints, 0, sizeof(hints));

    /*
     * hints.ai_family = AF_UNSPEC;
     *
     * これは
     *   「IPv4 にも IPv6 にも限定しない」
     * という意味である。
     *
     * AF_UNSPEC を指定すると、
     * getaddrinfo() は利用可能な候補として
     *   - IPv4
     *   - IPv6
     * のどちらも返せる。
     *
     * つまりこのコードは、
     *   「アドレス族に依存しにくい（protocol/address-family independent）」
     * 書き方をしている。
     *
     * もし IPv4 限定なら AF_INET、
     * IPv6 限定なら AF_INET6 を使う。
     */
    hints.ai_family = AF_UNSPEC;

    /*
     * hints.ai_socktype = SOCK_STREAM;
     *
     * これは
     *   「ストリーム型ソケットでつなぎたい」
     * という意味である。
     *
     * 通常は TCP 通信を意味すると考えてよい。
     *
     * ここで SOCK_STREAM を指定しているため、
     * getaddrinfo() は TCP 接続に使える候補を返す。
     */
    hints.ai_socktype = SOCK_STREAM;

    /*
     * getaddrinfo("www.titech.ac.jp", "http", &hints, &addrs)
     *
     * 第1引数:
     *   ホスト名
     *   -> "www.titech.ac.jp"
     *
     * 第2引数:
     *   サービス名
     *   -> "http"
     *
     *   ここで "http" と書くことで、
     *   OS は通常 TCP の 80 番ポートを意味するものとして解釈する。
     *
     *   つまり数値の "80" を直接書かなくてもよい。
     *
     * 第3引数:
     *   候補条件 hints
     *
     * 第4引数:
     *   結果の addrinfo リスト先頭を受け取るポインタ
     *
     * 戻り値:
     *   0 なら成功
     *   0 以外なら失敗
     *
     * 重要:
     *   getaddrinfo() のエラーは errno ではなく独自の戻り値で表される。
     *   そのため perror() ではなく gai_strerror() を使ってメッセージ化する。
     *
     * 成功すると addrs は連結リストの先頭を指し、
     * そこには例えば
     *   - IPv6 候補
     *   - IPv4 候補
     * などが順に並んでいることがある。
     *
     * このコードでは簡単のため、その先頭要素だけを使って接続している。
     *
     * 実務コードでは、
     *   先頭候補で socket()/connect() が失敗したら次候補へ進む
     * というループにすることが多い。
     */
    if ((cc = getaddrinfo("www.titech.ac.jp", "http", &hints, &addrs)) != 0) {
        /*
         * gai_strerror(cc)
         *
         * getaddrinfo() 系専用のエラーコード cc を、
         * 人間向け文字列へ変換する関数。
         *
         * perror() ではなくこちらを使う点が重要である。
         */
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(cc));
        exit(1);
    }

    /*
     * ------------------------------------------------------------
     * 2. 得られた候補の先頭要素を使って socket() を作る
     * ------------------------------------------------------------
     *
     * addrs は addrinfo の連結リスト先頭を指す。
     *
     * その先頭要素には、
     *   - ai_family
     *   - ai_socktype
     *   - ai_protocol
     *   - ai_addr
     *   - ai_addrlen
     * などが入っている。
     *
     * ここではその情報をそのまま socket() へ渡している。
     *
     * これが getaddrinfo() の利点の 1 つで、
     * 自分で IPv4 / IPv6 ごとの socket 引数を組み立てなくても、
     * 候補自身が必要情報を持っている。
     */

    /*
     * socket(addrs->ai_family, addrs->ai_socktype, addrs->ai_protocol)
     *
     * ai_family:
     *   AF_INET または AF_INET6 など
     *
     * ai_socktype:
     *   今回は SOCK_STREAM
     *
     * ai_protocol:
     *   通常は TCP に対応する値
     *
     * つまりこの socket() は
     *   「この候補に合ったソケットを作る」
     * という意味になる。
     *
     * 戻り値:
     *   成功時:
     *     新しいソケット fd
     *
     *   失敗時:
     *     -1
     */
    if ((s = socket(addrs->ai_family, addrs->ai_socktype, addrs->ai_protocol))
          == -1) {
        perror("socket");
        exit(1);
    }

    /*
     * ------------------------------------------------------------
     * 3. connect() でその候補へ接続する
     * ------------------------------------------------------------
     *
     * connect(s, addrs->ai_addr, addrs->ai_addrlen)
     *
     * 第1引数:
     *   接続に使うソケット fd
     *
     * 第2引数:
     *   接続先アドレス構造体（sockaddr* として見える）
     *
     * 第3引数:
     *   そのアドレス構造体のサイズ
     *
     * ここで重要なのは、
     * addrs->ai_addr がすでに getaddrinfo() によって
     * 「接続可能な形の sockaddr」になっていることだ。
     *
     * そのため、IPv4 か IPv6 かをこの場所では気にしなくてよい。
     *
     * connect() に成功すると、
     * ソケット s は「接続済みストリーム」になり、
     * 以後は read()/write() によってデータの送受信ができる。
     */
    if (connect(s, addrs->ai_addr, addrs->ai_addrlen) == -1) {
        perror("connect");
        exit(1);
    }

    /*
     * ------------------------------------------------------------
     * 4. addrinfo リストを freeaddrinfo() で解放する
     * ------------------------------------------------------------
     *
     * getaddrinfo() が返した addrs は動的に確保されたリストである。
     * したがって使い終わったら freeaddrinfo() で解放する必要がある。
     *
     * ここではすでに
     *   - socket() に必要な ai_family / ai_socktype / ai_protocol
     *   - connect() に必要な ai_addr / ai_addrlen
     * を使い終わって接続も確立したので、もう addrs は不要である。
     */
    freeaddrinfo(addrs);

    /*
     * ------------------------------------------------------------
     * 5. HTTP 要求を write() で送る
     * ------------------------------------------------------------
     *
     * 接続済みソケット s に対して、
     * HTTP リクエスト文字列をそのまま送る。
     *
     * ここでは
     *
     *   GET / HTTP/1.0\n\n
     *
     * を送っており、
     * 概念的には
     *   「トップページ / を HTTP/1.0 で取得したい」
     * という意味になる。
     *
     * ソケットも file descriptor の一種なので、
     * 接続済みであれば write() で送信できる。
     *
     * 注意:
     *   戻り値チェックや部分書き込みの考慮は省略されている。
     *   実務コードでは全量送信ループが望ましい。
     */
    write(s, httpreq, strlen(httpreq));

    /*
     * ------------------------------------------------------------
     * 6. サーバからの返答を read() で最後まで読む
     * ------------------------------------------------------------
     *
     * HTTP サーバはリクエストに対して、
     * ステータス行 + ヘッダ + 本文 を返してくる。
     *
     * それをこのコードでは、
     *   read() で受けて
     *   write(1, ...) でそのまま標準出力へ流す
     * 形にしている。
     *
     * 重要:
     *   TCP はバイトストリームであり、メッセージ境界を持たない。
     *   そのため 1 回の read() で全部届くとは限らない。
     *
     *   だから while ループで
     *     「届く限り読み続ける」
     * 必要がある。
     */
    while ((cc = read(s, buf, sizeof(buf))) > 0)
        /*
         * read() で得た cc バイトを、そのまま標準出力へ書く。
         *
         * 1 は STDOUT_FILENO と同じ意味であり、
         * 通常は端末画面である。
         *
         * printf("%s", buf) ではなく write() を使っているのは、
         * read() が返す buf が '\0' 終端の C 文字列とは限らないからである。
         *
         * したがって
         *   「何バイト読めたか」
         * という長さ情報 cc をそのまま使う write() が安全である。
         */
        write(1, buf, cc);

    /*
     * ここで cc == 0 ならサーバが送信を終えて接続を閉じたことを意味する。
     * cc < 0 なら read エラーである。
     *
     * 元コードでは read エラーのチェックは省略されている。
     * 実務コードなら if (cc < 0) perror("read"); などが欲しい。
     */

    /*
     * ------------------------------------------------------------
     * 7. shutdown() で通信を停止する
     * ------------------------------------------------------------
     *
     * shutdown(s, SHUT_RDWR)
     *
     * これはソケット通信の送受信両方を停止する操作である。
     *
     * SHUT_RDWR:
     *   読み取り方向も書き込み方向も両方止める
     *
     * close() は fd を解放する操作だが、
     * shutdown() はまず通信方向そのものを止める。
     *
     * 単純な短命クライアントでは close() だけでも十分な場合は多いが、
     * ここでは
     *   「通信停止」と「fd 解放」を分けて書いている。
     */
    shutdown(s, SHUT_RDWR);

    /*
     * ------------------------------------------------------------
     * 8. close() でソケット fd を解放する
     * ------------------------------------------------------------
     *
     * ここでプロセスが持つソケット fd を閉じる。
     *
     * つまり
     *
     *   shutdown():
     *     通信機能の停止
     *
     *   close():
     *     fd 資源の解放
     *
     * という役割分担になっている。
     */
    close(s);

    /*
     * ここまで来たということは、
     *   - 名前解決に成功した
     *   - 接続に成功した
     *   - HTTP 要求を送った
     *   - サーバ応答を最後まで受け取った
     *   - 後始末を終えた
     *
     * ということなので正常終了する。
     */
    return 0;
}
