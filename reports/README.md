# Báo cáo tiến độ đề tài chia sẻ tệp P2P

Cập nhật ngày 25/11/2026 cho nhánh `feature/client-core`. Báo cáo mô tả bản triển khai tại commit `26983ef`, gồm sửa lỗi hủy chia sẻ và cơ chế thử lại trên kết nối điều khiển. [Đặc tả giao thức](../docs/PROTOCOL.md) · [Hướng dẫn build và chạy](../README.md)

## 1. Kiến trúc và tiến độ theo phân công

Hệ thống viết bằng C11, dùng IPv4/TCP và pthread, với Ubuntu 20.04 là môi trường đích. Máy khách giữ một kết nối TCP điều khiển để làm việc với máy chủ. Máy chủ quản lý tài khoản, phiên và danh sách quảng bá. Khi người dùng chọn tải một tệp, máy khách mở một kết nối TCP riêng đến peer đã chọn và nhận dữ liệu trực tiếp từ peer đó.

### Cao Trường Xuân (20236016) · Máy chủ và mạng chung

| Hạng mục | Kết quả trên nhánh hiện tại |
| --- | --- |
| TCP và xử lý thông điệp | Máy chủ đa luồng xử lý lệnh theo dòng CRLF. Bộ đệm nhận xử lý được dòng bị chia nhỏ và nhiều lệnh đến trong cùng một lần đọc. |
| Tài khoản | Đăng ký tài khoản, lưu mật khẩu băm bằng PBKDF2-HMAC-SHA256 có salt và xác thực đăng nhập theo kết nối. |
| Phiên và danh sách peer | `HELLO` gắn ID, IPv4 nguồn và cổng P2P với phiên hiện tại. Đăng xuất hoặc ngắt kết nối giải phóng ID và các quảng bá của phiên. |
| Danh sách chia sẻ | Phiên có thể thêm, cập nhật và gỡ quảng bá của mình. Tìm kiếm theo tên chính xác trả về các peer phù hợp, xếp theo ID. |
| Nhật ký | Ghi thời gian UTC, vòng đời kết nối, trạng thái phản hồi và các thao tác tài khoản hoặc chia sẻ. |

### Nguyễn Đức Thắng (20235998) · Máy khách và truyền tệp P2P

| Hạng mục chấm | Điểm | Kết quả trên nhánh hiện tại |
| --- | ---: | --- |
| Giao tiếp qua socket | 2 | CLI trao đổi với máy chủ và peer qua TCP có bộ đệm. Bốn worker xử lý yêu cầu tải lên đồng thời. |
| Công bố ID và cổng P2P | 1 | Máy khách lưu ID 32 bit trong `config.txt`, gửi `HELLO` sau đăng nhập và làm mới đăng ký mỗi 10 giây. |
| Đăng ký chia sẻ | 2 | Máy khách lưu tên và đường dẫn vào `index.txt` trước khi công bố. Lệnh `shares` hiển thị trạng thái `ANNOUNCED` hoặc `PENDING`. |
| Hủy chia sẻ | 2 | Máy khách lưu việc xóa ở catalog trước khi yêu cầu máy chủ gỡ quảng bá, giữ trạng thái cũ nếu lưu lỗi và thử lại yêu cầu gỡ bị từ chối. |
| Tải tệp trực tiếp | 3 | Máy khách chọn peer từ kết quả tìm kiếm, đọc tệp nhị phân theo từng khung có độ dài và ghi vào tệp tạm cho đến khi nhận đủ khung cuối. |

## 2. Quy ước thông điệp

Cả kênh điều khiển và kênh giữa hai peer dùng IPv4/TCP. Các dòng lệnh, dòng trạng thái và dòng kết quả dùng ASCII in được và kết thúc bằng CRLF (`\r\n`). Tên lệnh phân biệt hoa và thường. Đúng một dấu cách ASCII phân tách các trường. Dòng rỗng, LF đứng riêng, CR ở giữa dòng, NUL, tab và dấu cách thừa đều là lỗi định dạng.

TCP chuyển một luồng byte. Một lần đọc có thể chỉ nhận một phần dòng hoặc nhận nhiều dòng liền nhau. Bên nhận giữ byte dư trong bộ đệm để lần đọc tiếp theo tiếp tục đúng vị trí. Máy chủ xử lý lệnh theo thứ tự và trả lời xong từng lệnh trước khi gửi phản hồi tiếp theo. Khi TCP đóng giữa một dòng, bên nhận bỏ dòng chưa hoàn chỉnh. Thời hạn 30 giây áp dụng cho khoảng không hoạt động ở mỗi thao tác socket.

| Giá trị | Biểu diễn trên đường truyền |
| --- | --- |
| Tên tài khoản | Một hoặc nhiều chữ cái ASCII, chữ số hoặc dấu gạch dưới. Phân biệt hoa và thường. |
| Mật khẩu | Ít nhất 8 byte trong khoảng ASCII từ `!` đến `~`, trong giới hạn kiểu `int` của OpenSSL. |
| ID máy khách | Số thập phân không dấu từ 0 đến 4.294.967.295. |
| Cổng peer | Số thập phân không dấu từ 1 đến 65.535. |
| Kích thước công bố | Số byte, từ 0 đến 18.446.744.073.709.551.615. |
| Tên tệp | Tên cơ sở phân biệt hoa và thường, dài 1–255 byte sau giải mã. |

Đầu vào số có thể có các số 0 ở đầu. Dấu âm, dấu cộng và giá trị tràn bị từ chối. Khi ghi số ra đường truyền, chương trình dùng dạng thập phân ngắn nhất.

Trong tên tệp, các chữ cái và chữ số ASCII cùng các ký tự `-`, `.`, `_`, `~` xuất hiện trực tiếp. Các byte ASCII in được còn lại dùng `%HH` với chữ số hex viết hoa. Ví dụ, `bao cao.bin` thành `bao%20cao.bin`, còn `100%.txt` thành `100%25.txt`. Bộ giải mã từ chối dấu gạch chéo, dấu gạch chéo ngược, tên `.` và `..`, mã hex viết thường và mã hóa phần ký tự được phép ghi trực tiếp.

## 3. Kết nối điều khiển với máy chủ

### 3.1. Lệnh và trạng thái phiên

| Lệnh | Điều kiện | Phản hồi thành công và tác dụng |
| --- | --- | --- |
| `REGISTER <username> <password>` | Chưa đăng nhập | `201`. Tạo tài khoản. Kết nối tiếp tục ở trạng thái chưa xác thực. |
| `LOGIN <username> <password>` | Chưa đăng nhập | `200`. Xác thực kết nối hiện tại. |
| `HELLO <client_id> <port>` | Đã đăng nhập | `200`. Gắn ID và cổng nhận tải với IPv4 nguồn của chính kết nối TCP. |
| `SHARE <filename> <size>` | Đã đăng nhập và `HELLO` thành công | `200`. Thêm hoặc cập nhật quảng bá theo tên và kích thước. |
| `SEARCH <filename>` | Đã đăng nhập và `HELLO` thành công | Từ 0 dòng `FILE` trở lên, kết thúc bằng `200 END`. |
| `UNSHARE <filename>` | Đã đăng nhập và `HELLO` thành công | `200`. Gỡ quảng bá do phiên gửi sở hữu. |
| `LOGOUT` | Đã đăng nhập | `200`. Giải phóng ID và quảng bá. Kết nối TCP tiếp tục mở để đăng nhập lại. |

Một ID đang hoạt động thuộc về một kết nối điều khiển. Cùng một tài khoản có thể có nhiều phiên với các ID khác nhau. Khi cùng phiên gửi lại `HELLO` với ID cũ và cổng mới, máy chủ giữ quảng bá và cập nhật cổng trả về trong kết quả tìm kiếm. Khi đổi sang ID mới thành công, máy chủ xóa quảng bá của ID cũ. Một lần `HELLO` bị từ chối giữ nguyên đăng ký trước đó.

### 3.2. Mã phản hồi thành công và lỗi

Mỗi dòng phản hồi dưới đây kết thúc bằng CRLF trên đường truyền. Phản hồi lỗi điều khiển chỉ gồm mã ba chữ số và CRLF. Máy chủ kiểm tra cú pháp và giá trị trước khi kiểm tra trạng thái phiên. Một lệnh có quá hai đối số nhận `400`.

| Dòng phản hồi | Ý nghĩa |
| --- | --- |
| `200` | Lệnh thực hiện thành công. |
| `201` | `REGISTER` đã tạo tài khoản. Kết nối tiếp tục ở trạng thái chưa đăng nhập. |
| `400` | Định dạng dòng, cú pháp, số lượng đối số hoặc giá trị không hợp lệ. |
| `401` | Cần đăng nhập hoặc thông tin đăng nhập sai. Tài khoản không tồn tại và mật khẩu sai nhận cùng mã. |
| `403` | Phiên cần `HELLO` thành công trước khi dùng lệnh quản lý tệp. |
| `404` | Phiên gửi `UNSHARE` không có quảng bá tương ứng. Trên kênh P2P, mã này báo tệp được yêu cầu không khả dụng. |
| `409` | Tài khoản đã tồn tại, kết nối đã đăng nhập hoặc ID đã được phiên khác sử dụng. |
| `500` | Lỗi xử lý nội bộ, chẳng hạn lỗi lưu tài khoản, băm mật khẩu hoặc cấp phát bộ nhớ. |
| `501` | Lệnh có định dạng hợp lệ nhưng tên lệnh chưa được định nghĩa. |
| `503` | Máy chủ không tạo được phiên hoặc worker và đóng kết nối. |

### 3.3. Tìm kiếm và chọn peer

Máy chủ khớp toàn bộ tên tệp, kể cả hoa và thường, rồi lấy một bản chụp các quảng bá hiện có theo thứ tự ID tăng dần. Nhiều peer có thể chia sẻ cùng tên. Chính peer gửi yêu cầu cũng có thể nằm trong kết quả. Một peer có thể ngắt kết nối sau thời điểm máy chủ lấy bản chụp.

Ví dụ, yêu cầu đầu tiên dưới đây trả về hai địa chỉ. Mỗi dòng trên đường truyền kết thúc bằng CRLF.

```text
SEARCH bao%20cao.bin
FILE bao%20cao.bin 5 12 192.168.1.21 9001
FILE bao%20cao.bin 9 34 192.168.1.22 9002
200 END
```

Dòng `FILE` lần lượt mang tên đã mã hóa, kích thước khi công bố, ID, IPv4 và cổng P2P. Tìm kiếm rỗng trả ngay `200 END`. Tìm kiếm bị từ chối trả một dòng mã lỗi đầu tiên. Máy khách kiểm tra toàn bộ phản hồi trước khi hiển thị kết quả có thể chọn. Nếu kết nối đóng trước `200 END`, máy khách bỏ các dòng đã nhận.

CLI giải mã tên tệp và đánh số mỗi kết quả. Người dùng nhập `search bao cao.bin`, sau đó có thể nhập `download 1` để kết nối tới `192.168.1.21:9001`. Danh sách kết quả được xóa trước lần tìm kiếm tiếp theo và khi đăng xuất.

## 4. Kết nối truyền tệp giữa hai peer

### 4.1. Yêu cầu và phản hồi

Bên tải mở TCP trực tiếp đến IPv4 và cổng đã chọn rồi gửi `GET <filename>\r\n`. Bên cung cấp tra catalog và chỉ mở tệp thường đã được công bố thành công trên phiên điều khiển đang hoạt động. Nếu chấp nhận, nó gửi `200 CHUNKED\r\n` và bắt đầu truyền các khung. Mỗi kết nối tải phục vụ một tệp. Dòng văn bản giữa hai peer dài tối đa 771 byte, tính cả CRLF.

Cổng P2P chấp nhận yêu cầu `GET` từ máy có thể kết nối đến nó. Giao thức này dành cho mạng được kiểm soát.

### 4.2. Length prefix và khung cuối

`length` ghi **số byte nội dung tệp của khung hiện tại**. Việc đếm bắt đầu ở byte đầu tiên sau CRLF của header. Các ký tự `<data>` và `<end>` xuất hiện nguyên dạng trên đường truyền. Giá trị độ dài được viết bằng số thập phân không dấu.

| Header gửi trên TCP | Giá trị `length` | Cách bên nhận xử lý |
| --- | --- | --- |
| `<data> <length>\r\n` | 1–65.536 byte | Đọc đúng `length` byte nội dung, rồi đọc header tiếp theo. |
| `<end> <length>\r\n` | 0–65.536 byte | Đọc đúng `length` byte nội dung, rồi kết thúc lượt tải. Tệp rỗng dùng `<end> 0\r\n`. |

Ví dụ sau là một chuỗi byte hợp lệ để truyền tệp `abcde`. Các ký hiệu `\r\n` biểu diễn đúng hai byte CR và LF.

```text
200 CHUNKED\r\n<data> 3\r\nabc<end> 2\r\nde
```

1. Bên nhận đọc `200 CHUNKED\r\n` để xác nhận định dạng phản hồi.
2. Nó đọc `<data> 3\r\n` rồi lấy đúng ba byte `abc`. Byte kế tiếp là dấu `<` mở đầu header mới.
3. Nó đọc `<end> 2\r\n`, lấy đúng hai byte `de` và kết thúc lượt nhận. Tổng nội dung lưu xuống là `3 + 2 = 5` byte.

Payload có thể chứa mọi giá trị byte, kể cả CRLF, NUL hoặc chuỗi `<end>`. Bên nhận xác định ranh giới bằng cách đếm byte theo header. Một bộ đọc có đệm phục vụ cả header và payload để giữ đúng vị trí khi TCP tách hoặc gộp dữ liệu.

### 4.3. Điều kiện hoàn tất và lỗi giữa chừng

- **Bên gửi.** Đọc tệp nguồn tới EOF thực tế rồi đánh dấu khung cuối bằng `<end>`. Byte mới được nối vào trước EOF có thể xuất hiện trong lượt tải. Việc cắt tệp có thể làm EOF đến sớm. Byte đã đọc vẫn có thể được gửi nếu nguồn bị sửa hoặc bị xóa sau đó.
- **Bên nhận.** Tính kích thước thực nhận bằng tổng payload của tất cả các khung. Kích thước trong dòng `FILE` phản ánh thời điểm công bố. Khi đọc đủ payload của `<end>`, bên nhận hoàn tất lượt tải và đóng socket. Dữ liệu dư sau khung cuối bị bỏ.
- **Lỗi trước dữ liệu.** Bên cung cấp có thể trả `400`, `404` hoặc `501` rồi đóng kết nối. Nếu xảy ra lỗi đọc nguồn trong khi truyền, nó đóng kết nối trước khung cuối.
- **Lỗi khi nhận.** Header sai cú pháp, kết nối đóng sớm hoặc quá thời hạn chờ làm lượt tải thất bại. Client xóa tệp tạm. Khi lượt tải thành công, client đồng bộ dữ liệu và công bố tệp đích mà không ghi đè đường dẫn đã tồn tại.

## 5. Vòng đời chia sẻ và phục hồi

| Dữ liệu | Nơi lưu | Vòng đời |
| --- | --- | --- |
| Tài khoản | Kho tài khoản trên máy chủ | Giữ qua lần khởi động lại máy chủ. |
| ID đang hoạt động, IPv4, cổng và quảng bá | Bộ nhớ máy chủ | Thuộc về phiên TCP. Đăng xuất hoặc mất kết nối giải phóng ID và các quảng bá trong cùng lần cập nhật. |
| ID máy khách | `config.txt` | Dùng lại khi khởi động lại máy khách. |
| Tên và đường dẫn nguồn | `index.txt` | Mỗi dòng có tên và đường dẫn tuyệt đối mã hóa hex, phân tách bằng tab và kết thúc bằng LF. Tệp khóa riêng bảo vệ thao tác ghi. |
| Yêu cầu gỡ quảng bá đang chờ | Bộ nhớ máy khách | Tồn tại trong phiên hiện tại để thử lại. Catalog đã lưu quyết định có công bố lại sau khởi động hay không. |

Khi người dùng chạy `share`, máy khách lưu catalog trước rồi gửi `SHARE` nếu đã có phiên hoạt động. Một mục là `ANNOUNCED` khi máy chủ xác nhận công bố. Tệp thiếu hoặc yêu cầu công bố bị từ chối có thể ở lại catalog dưới trạng thái `PENDING`. Sau đăng nhập hoặc một lệnh `hello` thủ công thành công, máy khách thử công bố lại các mục còn đọc được. Keepalive gửi lại `HELLO` mỗi 10 giây.

| Tình huống khi chạy `unshare` | Kết quả |
| --- | --- |
| Lưu catalog thất bại | Giữ mục trong catalog, quyền phục vụ upload và quảng bá của phiên. CLI báo lỗi lưu. |
| Lưu catalog thành công | Mục biến mất khỏi catalog và lượt tải mới bị chặn. Lượt tải đã mở tệp có thể tiếp tục. Nếu có phiên, client gửi `UNSHARE`. |
| Máy chủ trả `200` hoặc `404` | Client xác nhận quảng bá của phiên đã vắng mặt. |
| Máy chủ từ chối nhưng kết nối còn hoạt động | Client giữ yêu cầu trong bộ nhớ và thử lại trong chu kỳ keepalive hoặc sau một `hello` thủ công thành công. |
| Có `SHARE` mới cùng tên | Nếu thành công, quảng bá mới thay thế quảng bá cũ và yêu cầu gỡ cũ bị hủy. Cùng một khóa tuần tự hóa các trao đổi điều khiển. |
| Mất kết nối trước khi có phản hồi | Máy chủ dọn quảng bá của phiên. Catalog đã lưu việc xóa, nên lần đăng nhập sau chỉ công bố các mục còn lại. |

Máy khách kết thúc phiên điều khiển và chặn lượt tải lên mới khi nhận phản hồi sai định dạng hoặc gặp lỗi truyền trên kết nối đó. Nếu peer không mở được nguồn khi nhận `GET`, nó trả `404`. CLI bên tải hiển thị lỗi cục bộ. Bên chia sẻ có thể làm mới bằng `hello` hoặc gỡ mục bằng `unshare`.

Nhật ký máy chủ mặc định ở `<accounts-path>.log` và có thể đổi bằng `--log PATH`. Log là tệp thường có quyền `0600`. Log ghi thời gian UTC, kết nối, mã phản hồi, tên tài khoản khi thao tác thành công, thông tin công bố và thao tác tệp. Mật khẩu và đầu vào lỗi thô được loại khỏi log. Máy chủ yêu cầu log khả dụng để khởi động. Trong lúc chạy, máy chủ tiếp tục xử lý yêu cầu nếu gặp lỗi ghi log.

## 6. Phiên bản và kiểm thử

Server và client được build từ cùng commit để dùng chung đặc tả điều khiển. Hai peer dùng cùng định dạng `200 CHUNKED`, `<data>` và `<end>` khi trao đổi tệp. Mốc đối chiếu `267fbf0` trên `main` có giao thức riêng với `SHARE <client_id> <filename> <size>`, kết quả `200-...` kèm revision, lệnh `REPORT` và header truyền tệp `200 <size>`. Checklist đề xuất bổ sung phản hồi `200 <revision>` cho `SHARE` và báo cáo từ uploader trên thiết kế ở mốc đó.

| Kiểm thử đã chạy | Kết quả |
| --- | --- |
| `make test` trong Docker Ubuntu 20.04 | Qua 3 bộ test C và 86 test Python, gồm ca từ chối quyền truy cập kho tài khoản. |
| Hồi quy `unshare` | Xác nhận lỗi ghi giữ trạng thái hai phía, tự thử lại khi server từ chối, giữ quảng bá mới khi chia sẻ lại và giữ việc xóa qua khởi động lại khi mất phản hồi. |
| Build đồng bộ server và client từ commit `aae16f8` | Qua 5 bài kiểm thử tích hợp trên Ubuntu 20.04, gồm tải tệp nhị phân, tải tệp rỗng, tìm kiếm và vòng đời phiên. |
| LAN mô phỏng trên ba container Ubuntu 20.04 | Một server và hai peer có IPv4 riêng. Hai client dùng hai tài khoản, tìm đúng IPv4 và cổng, tải khớp từng byte tệp 1.048.832 byte và xác nhận `unshare` chỉ gỡ quảng bá của peer gửi lệnh. |

Nghiệm thu trên hai máy trạm Ubuntu 20.04 qua mạng LAN thực tế vẫn còn. Các tình huống cần kiểm tra thêm gồm mất kết nối điều khiển khi nhập mật khẩu, Ctrl+C đúng lúc đồng bộ và công bố tệp đích, và mất kết nối điều khiển khi lượt tải đang chờ peer gửi dữ liệu.
