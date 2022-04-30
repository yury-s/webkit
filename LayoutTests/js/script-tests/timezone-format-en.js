const date1 = new Date(1651263043690);

shouldBeEqualToString("date1.toString()", "Fri Apr 29 2022 16:10:43 GMT-0400 (Eastern Daylight Time)");
shouldBeEqualToString("date1.toTimeString()", "16:10:43 GMT-0400 (Eastern Daylight Time)");
shouldBeEqualToString("Intl.DateTimeFormat().resolvedOptions().timeZone", "America/New_York");
