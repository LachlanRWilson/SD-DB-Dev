#set page(paper: "a4", flipped: true, margin: 1.5cm)
#set text(size: 9pt)
#set par(justify: false)

#let group(name) = table.cell(colspan: 4, fill: luma(225), text(weight: "bold", name))

= Database Interface Requirements: UniQOS Access Points

Each row is a point in the UniQOS code base (as of 09/10/2026) that needs database access, with the data the UI sends to the database task and the data it needs back. All operations are keyed by phone number.

#table(
  columns: (1.4fr, 1fr, 1fr, 1.2fr),
  align: left,
  inset: 5pt,
  stroke: 0.5pt + luma(160),
  table.header(
    [*Code location*], [*Trigger*], [*Input (UI → DB)*], [*Output (DB → UI)*],
  ),

  group[Save contact],
  [`ui/pages/contacts/contact_details.c:115` ("Edit Contact")], [User edits an existing contact], [`name`, `phone`], [Status],
  [`ui/pages/phone/call.c:44` ("Add to Contacts")], [User saves the number from a call], [`name`, `phone`], [Status],

  group[Delete contact],
  [`ui/pages/contacts/contact_details.c:118` ("Delete Contact")], [User deletes a contact], [`phone`], [Status],

  group[Get contact (by phone)],
  [`kernel/tasks/mm_task.c:156` (`handle_incoming_call`) → `ui/overlays/incoming_call.c`], [Incoming call], [`phone`], [`name` (caller ID)],
  [`kernel/tasks/mm_task.c:194` (`handle_show_sms`) → `ui/overlays/incoming_text.c`], [Incoming SMS notification], [`phone`], [`name` (sender)],
  [`ui/pages/sms/messages.c:49` ("From:" header)], [Conversation opened], [`phone`], [`name`],
  [`ui/pages/contacts/contacts.c:143` (`INPUT_SELECT`)], [Contact selected from list], [`phone`], [`name`, `phone`],

  group[Save message (append)],
  [`kernel/tasks/mm_task.c:299` (`PAGE_REQUEST_SMS_SEND`) ← `ui/pages/sms/new_sms.c:51`], [SMS sent], [`phone`, `body`, `direction = sent`, `timestamp`], [Status],
  [`kernel/tasks/mm_task.c:194` (`handle_show_sms`) ← `kernel/tasks/cellular_task.c:226`], [SMS received], [`phone`, `body`, `direction = received`, `timestamp`], [Status],

  group[Get message (by phone)],
  [`kernel/tasks/mm_task.c:58` (`incoming_text_callback`, OPEN)], [User opens a new SMS], [`phone`], [Opens the conversation (message list for `phone`)],
  [Conversation list rows (new page, from "All Messages")], [Preview of latest message], [`phone`], [Latest `body`, `timestamp`],

  group[Get list],
  [`ui/pages/contacts/contacts.c:17–53, 103, 185` (hardcoded `names[]` / `phones[]`, `max_y = 15`)], [Contacts page opened / scrolled past 9 rows], [`start`, `count = 9`], [≤ 9 × {`name`, `phone`}, total count],
  [`ui/pages/sms/messages.c` (single message in `MessagePageState`)], [Conversation opened / scrolled], [`phone`, `start`, `count = N`], [N × {`body`, `direction`, `timestamp`}, newest first],
  [`ui/pages/sms/sms.c:131` ("All Messages", hardcoded `"1234567890"`)], [SMS menu option selected], [`start`, `count`], [List of {`phone`, `name`, latest preview, `timestamp`}, most recent first],
  [`ui/pages/phone/call_logs.c` (empty file)], [Recent calls page (future)], [`start`, `count`], [List of {`phone`, `name`, `direction`, `timestamp`}],
)

#v(6pt)
*Test code to remove, not wire up:* `ui/pages/sms/sms.c:139` ("Favourites") and `ui/pages/phone/phone.c:142` both raise fake overlays for the test number `"1234567890"`.
